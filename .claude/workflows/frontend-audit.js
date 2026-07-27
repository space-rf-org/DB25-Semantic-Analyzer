export const meta = {
  name: 'frontend-audit',
  description: 'Adversarial correctness audit of the DB25 SQL frontend across all four layers (tokenizer -> parser -> analyzer -> binder/optimizer), with a regression-audit of the most recent changes. Run after landing anything major.',
  whenToUse: 'After merging a significant change to any DB25 frontend layer, to catch new defects and regressions introduced by the change before they compound up the cascade.',
  phases: [
    { title: 'Audit', detail: 'one adversarial agent per layer + a cross-layer sweep, in parallel' },
  ],
}

// ---------------------------------------------------------------------------
// Repeatable, EVERGREEN adversarial audit. It deliberately hardcodes NO list of
// "already fixed" defects (that would rot): each agent instead (a) builds the
// suite green first, (b) treats anything a regression test already pins as
// KNOWN, and (c) regression-audits the most recent commits (`git log`). So the
// same script keeps finding new/introduced defects as the codebase evolves.
//
// Each agent returns structured findings; the main loop consolidates, verifies,
// and fixes. Reproduced defects only.
// ---------------------------------------------------------------------------

const FINDINGS_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  properties: {
    findings: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        properties: {
          summary: { type: 'string', description: 'one-line statement of the defect' },
          location: { type: 'string', description: 'file:line of the root cause' },
          repro: { type: 'string', description: 'minimal SQL (or steps) that reproduces it' },
          observed: { type: 'string', description: 'actual behaviour (dumped AST/plan, sanitizer trace, wrong value)' },
          expected: { type: 'string', description: 'correct behaviour (cite Postgres for semantics)' },
          severity: {
            type: 'string',
            enum: ['crash-or-ub', 'wrong-result', 'wrong-plan-or-slot', 'non-idempotence', 'minor'],
          },
          regression_of: { type: 'string', description: 'commit/PR this appears to regress, or "" if pre-existing' },
        },
        required: ['summary', 'repro', 'observed', 'expected', 'severity'],
      },
    },
    stress_tested_clean: {
      type: 'array',
      items: { type: 'string' },
      description: 'what was attacked and held up (for confidence)',
    },
  },
  required: ['findings'],
}

// Shared adversarial preamble, specialized per layer below.
const COMMON = `You are running a SECOND-/THIRD-PASS ADVERSARIAL correctness audit. Assume subtle defects remain. REPRODUCED defects only.

METHOD (mandatory):
1. Clone fresh into the scratchpad path given below, init submodules, and get the existing test suite GREEN first. Note the build/sanitizer flags the CI uses and mirror them for repros (-fsanitize=address,undefined -fno-sanitize-recover=all where applicable).
2. Run \`git log --oneline -25\` and REGRESSION-AUDIT the most recent changes specifically: attack the newly-changed code paths and their edges hard, looking for defects the change introduced.
3. For every SUSPECTED defect, construct a minimal repro, run it, and CONFIRM (sanitizer error, or actual-vs-expected AST/plan/diagnostic captured). Do not report anything you have not reproduced.

DO NOT REPORT (evergreen filters, so this stays useful as the code evolves):
- Behaviour a regression test already pins (that is KNOWN/handled - grep the tests before reporting).
- Missing SQL features / dialect gaps, UNLESS they cause a crash, memory error, UB, or a SILENTLY-WRONG result (wrong AST/type/nullability/plan/slot, a legal query rejected, or an illegal query silently accepted).
- Deliberate, documented design choices (read the surrounding comments; if it is clearly intentional and internally consistent, note it but do not rank it as a defect).
- Lenient leftover-token tolerance in the parser (by design).

Rank findings crash-or-ub > wrong-result > wrong-plan-or-slot > non-idempotence > minor. Prefer a few high-confidence findings over a long speculative list. If you find nothing new, return an empty findings array and list exactly what you attacked in stress_tested_clean.`

const LAYERS = [
  {
    key: 'parser',
    dir: 'audit_parser',
    prompt: `${COMMON}

LAYER: **db25-sql-parser** (C++23, -fno-exceptions, recursive-descent + Pratt). Clone https://github.com/space-rf-org/db25-sql-parser.git into /tmp/claude-0/-home-user-DB25-Semantic-Analyzer/12265417-efc3-5183-bc33-f76593ee1902/scratchpad/audit_parser, \`git submodule update --init --recursive\`, build Debug (cmake), run ctest. API: \`db25::parser::Parser p; auto r = p.parse(sv);\` -> ast::ASTNode*.

ATTACK: depth-guard completeness on EVERY recursive parse_* entry (drive ~100k-500k deep, expect graceful "Maximum recursion depth exceeded", never a crash); operator precedence/associativity across the full matrix; silent mis-parses (a clause/keyword consumed wrong or dropped while the parse still succeeds); memory safety via a large fuzz of malformed/truncated/random-byte/unbalanced inputs under ASan/UBSan; numeric-literal edge handling. Regression-audit whatever the latest commits changed.`,
  },
  {
    key: 'analyzer',
    dir: 'audit_analyzer',
    prompt: `${COMMON}

LAYER: **DB25-Semantic-Analyzer** (C++23, -fno-exceptions; name resolution/scopes, type inference & coercion, catalog, CHECK constant eval, aggregate/grouping validation, set-op reconciliation, nullability). Clone https://github.com/space-rf-org/db25-semantic-analyzer.git into /tmp/claude-0/-home-user-DB25-Semantic-Analyzer/12265417-efc3-5183-bc33-f76593ee1902/scratchpad/audit_analyzer, init submodules, build, ctest green. Read tests/ for the API (Analyzer(catalog).analyze(ast); diagnostics()/type_of/nullability_of; evaluate_check for CHECK). Postgres is the semantic reference.

ATTACK: type/nullability matrix corners (CASE/COALESCE/NULLIF/GREATEST/LEAST, mixed-numeric arithmetic, aggregate/window nullability, NULL propagation, IN/BETWEEN); name resolution & scopes (correlated depth>=2, alias visibility per clause, self-joins with 3+ copies, CTE shadowing, USING/NATURAL coalescing); aggregate/grouping legality; set-op arity/type/nullability reconciliation across 3+ branches; the CHECK constant evaluator's three-valued logic and integer overflow/UB; its own recursion bounding. Regression-audit the latest commits.`,
  },
  {
    key: 'logical-plan',
    dir: 'audit_lp',
    prompt: `${COMMON}

LAYER: **db25-logical-plan** (C++23, -fno-exceptions; binder AST->plan with numbered slots + optimizer). Clone https://github.com/space-rf-org/db25-logical-plan.git into /tmp/claude-0/-home-user-DB25-Semantic-Analyzer/12265417-efc3-5183-bc33-f76593ee1902/scratchpad/audit_lp, init submodules, build, ctest green (3 suites). API: parse->analyze->Binder(analyzer,cat).bind(ast)->BindResult; optimize(std::move(root)); dump_plan/dump_expr to inspect.

INVARIANTS to attack: (i) every optimizer expression walker must traverse EVERY sub-expression holding a slot ref (children, window PARTITION/ORDER BY, aggregate FILTER, subquery boundaries) - a missed branch => stale/OOB slot; (ii) optimize(optimize(p)) must equal optimize(p) structurally. ATTACK: wrong slot binding across joins/self-joins/USING/NATURAL/correlated-subqueries/CTEs; result-changing optimizer rewrites (predicate pushdown below the null-supplying side of an outer join, NOT IN with NULLs, dead-conjunct elimination, column pruning dropping a still-referenced slot); non-idempotence on stacked/nested plans; binder/optimizer recursion bounding (deep expr chains, recursive CTEs). Regression-audit the latest commits.`,
  },
  {
    key: 'cross-layer',
    dir: 'audit_xlayer',
    prompt: `${COMMON}

FOCUS: **CROSS-LAYER integration + end-to-end memory safety.** db25-logical-plan is the integration point (it pins parser + analyzer as submodules and drives the whole pipeline). Clone https://github.com/space-rf-org/db25-logical-plan.git into /tmp/claude-0/-home-user-DB25-Semantic-Analyzer/12265417-efc3-5183-bc33-f76593ee1902/scratchpad/audit_xlayer, init submodules, build, ctest green. Write an end-to-end driver (model it on tests/test_binder.cpp) that runs a SQL string through parse -> analyze -> bind -> optimize -> optimize(optimize) under ASan/UBSan.

ATTACK the SEAMS, not per-layer issues the other agents own: an input the parser accepts but the analyzer reads with a wrong AST assumption; something the analyzer marks resolved/typed that the binder then binds to the wrong slot or fails to bind (an analyzer<->binder disagreement); a node shape reachable only through the REAL parser that the binder/optimizer mishandles; end-to-end non-idempotence for a real query. Drive a broad corpus (all join kinds, set-ops incl. leading-paren/nested, correlated subqueries depth>=2, GROUP BY/HAVING/window/FILTER, VALUES, DML+subqueries, CTEs, DDL) plus malformed inputs to prove the whole pipeline degrades gracefully rather than crashing downstream. Regression-audit the latest commits across all three pinned layers.`,
  },
]

log(`frontend-audit: launching ${LAYERS.length} adversarial agents in parallel`)

const results = await parallel(
  LAYERS.map((L) => () =>
    agent(L.prompt, {
      label: `audit:${L.key}`,
      phase: 'Audit',
      agentType: 'general-purpose',
      schema: FINDINGS_SCHEMA,
    }).then((r) => ({ layer: L.key, ...(r || { findings: [], stress_tested_clean: [] }) }))
  )
)

// Flatten, tag each finding with its layer, and rank by severity for the main
// loop to verify + fix.
const SEV = { 'crash-or-ub': 0, 'wrong-result': 1, 'wrong-plan-or-slot': 2, 'non-idempotence': 3, 'minor': 4 }
const findings = results
  .filter(Boolean)
  .flatMap((r) => (r.findings || []).map((f) => ({ ...f, layer: r.layer })))
  .sort((a, b) => (SEV[a.severity] ?? 9) - (SEV[b.severity] ?? 9))

log(`frontend-audit: ${findings.length} reproduced finding(s) across ${results.filter(Boolean).length} layers`)

return {
  findings,
  clean: results.filter(Boolean).map((r) => ({ layer: r.layer, stress_tested_clean: r.stress_tested_clean || [] })),
}
