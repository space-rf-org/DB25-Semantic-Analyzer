export const meta = {
  name: 'frontend-audit',
  description: 'Adversarial correctness audit of the DB25 SQL frontend. One boundary-respecting agent per pipeline stage (tokenizer/parser -> analyzer -> binder/optimizer) plus a contract-seam auditor; each stays inside its repo and surfaces cross-stage issues as contract negotiations. Run after landing anything major.',
  whenToUse: 'After merging a significant change to any DB25 frontend stage, to catch new defects and regressions before they compound up the cascade - and to catch input/output CONTRACT drift between stages before one stage silently absorbs another stage\'s bug.',
  phases: [
    { title: 'Audit', detail: 'one agent per repo/stage + a contract-seam auditor, in parallel' },
  ],
}

// ---------------------------------------------------------------------------
// Repeatable, EVERGREEN adversarial audit. Hardcodes NO list of "already fixed"
// defects (that would rot): each agent (a) builds its suite green first,
// (b) treats anything a regression test already pins as KNOWN, and
// (c) regression-audits the most recent commits (`git log`).
//
// DESIGN: the DB25 frontend is a staged pipeline
//     tokenizer -> parser -> analyzer -> binder/logical-plan(+optimizer)
// where each stage is a pure input->output transform across a repo boundary.
// Exactly ONE agent owns each stage/repo; a fifth agent audits the SEAMS (the
// inter-stage contracts) only. Every agent is held to a strict BOUNDARY
// DISCIPLINE (see COMMON): fix only your own stage's internals; never reach into
// another repo; never absorb an upstream bug by mangling your own output, and
// never mask your own bug by demanding a different input. A defect whose real
// fix lives across a boundary is reported as a CONTRACT finding naming the
// counterparty stage, to be negotiated - not cascaded.
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
          location: { type: 'string', description: 'file:line of the root cause (in YOUR repo, or the seam)' },
          repro: { type: 'string', description: 'minimal SQL (or steps) that reproduces it' },
          observed: { type: 'string', description: 'actual behaviour (dumped AST/plan, sanitizer trace, wrong value)' },
          expected: { type: 'string', description: 'correct behaviour (cite Postgres for semantics)' },
          severity: {
            type: 'string',
            enum: ['crash-or-ub', 'wrong-result', 'wrong-plan-or-slot', 'non-idempotence', 'minor'],
          },
          // BOUNDARY of the fix, per the discipline below:
          //  internal        - fix lives entirely inside THIS stage's own code.
          //  input-contract  - root cause is that this stage's INPUT violates the
          //                    contract it relies on; the fix belongs UPSTREAM.
          //                    Do NOT self-patch or alter your own output for it.
          //  output-contract - the correct fix would change what THIS stage EMITS
          //                    to its downstream; the output contract is consumed
          //                    downstream, so it is a negotiation, not a unilateral
          //                    edit.
          boundary: {
            type: 'string',
            enum: ['internal', 'input-contract', 'output-contract'],
          },
          // For input-contract / output-contract: the upstream (resp. downstream)
          // stage the contract change must be negotiated with. "" for internal.
          counterparty_stage: { type: 'string' },
          regression_of: { type: 'string', description: 'commit/PR this appears to regress, or "" if pre-existing' },
        },
        required: ['summary', 'repro', 'observed', 'expected', 'severity', 'boundary'],
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

// Shared adversarial preamble + the boundary discipline, specialized per stage.
const COMMON = `You are running an ADVERSARIAL correctness audit of ONE stage of the DB25 SQL frontend. Assume subtle defects remain. REPRODUCED defects only.

THE PIPELINE (each arrow is a repo boundary; each stage is a pure input->output transform):
  tokenizer  --token stream-->  parser  --AST (ast::ASTNode tree)-->  analyzer  --AST annotated with context.analysis (table_id, column_id, type, nullability) + diagnostics-->  binder/logical-plan  --LogicalPlan with numbered slots-->  (executor, out of scope)

BOUNDARY DISCIPLINE (mandatory - this is the point of the audit):
You OWN exactly one stage (stated below). Your INPUT contract is defined by your upstream's OUTPUT; your OUTPUT contract is what your downstream consumes. The two contracts are INDEPENDENT and each is owned by exactly one side.
- You may only propose fixes WITHIN your own repo/stage, changing your stage's INTERNALS. Never edit, or design a fix that requires editing, another repo.
- If a defect's root cause is that your INPUT violates the contract you rely on (your upstream emitted a wrong or mis-shaped artifact), DO NOT fix it in your stage and DO NOT paper over it by changing your own output (your output is coupled to YOUR downstream). Report it with boundary="input-contract" and counterparty_stage=<your upstream>: the fix belongs upstream, and any change to the input contract is a negotiated change to UPSTREAM'S OUTPUT.
- If the only correct fix would require you to change what you EMIT to your downstream, report it with boundary="output-contract" and counterparty_stage=<your downstream>: your output is consumed downstream, so changing its shape/meaning is a bilateral negotiation, never a unilateral edit.
- Invariant: a change to your OUTPUT must never silently change what you EXPECT as INPUT, and a needed change to your INPUT must never force a change to your OUTPUT. Cross-boundary changes are negotiated between the two adjacent stages only - never cascaded through the pipeline.
- Everything else - the fix is entirely inside your stage - is boundary="internal".

METHOD (mandatory):
1. Get a CURRENT, CLEAN checkout at the scratchpad path given below, then get the existing test suite GREEN. The scratchpad PERSISTS across audit runs, so a stale clone from a previous pass may already be sitting at that path - auditing it re-reports bugs that were fixed on main weeks ago. You MUST hard-sync to the latest default branch, never trust a pre-existing checkout:
   - If the directory does not exist: \`git clone <url> <path>\`.
   - If it DOES exist: \`cd <path> && git fetch origin && git checkout main && git reset --hard origin/main && git clean -fdx -e build\* \` (do NOT skip this - a failed \`git clone\` over an existing dir leaves the OLD commit in place).
   - Then ALWAYS: \`git submodule sync --recursive && git submodule update --init --recursive --force\` so pinned submodules match the synced main.
   - VERIFY before auditing: run \`git log --oneline -1\` and \`git rev-parse HEAD\`, and confirm it equals origin/main (\`git rev-parse origin/main\`). State the HEAD sha in your report. If they differ, STOP and re-sync - do not audit a stale tree.
   Build in a FRESH build directory (e.g. \`rm -rf build-audit && cmake -S . -B build-audit ...\`) so no stale object files from an earlier pass survive a source change. Mirror the CI build/sanitizer flags for repros (-fsanitize=address,undefined -fno-sanitize-recover=all where applicable).
2. Run \`git log --oneline -25\` and REGRESSION-AUDIT the most recent changes specifically: attack the newly-changed code paths and their edges hard.
3. For every SUSPECTED defect, construct a minimal repro, run it, and CONFIRM (sanitizer error, or actual-vs-expected AST/plan/diagnostic captured). Report nothing you have not reproduced. Classify each finding's boundary honestly per the discipline above.

DO NOT REPORT (evergreen filters):
- Behaviour a regression test already pins (grep the tests before reporting).
- Missing SQL features / dialect gaps, UNLESS they cause a crash, memory error, UB, or a SILENTLY-WRONG result (wrong AST/type/nullability/plan/slot, a legal query rejected, or an illegal query silently accepted).
- Deliberate, documented design choices (read the surrounding comments).
- Lenient leftover-token tolerance in the parser (by design).

Rank crash-or-ub > wrong-result > wrong-plan-or-slot > non-idempotence > minor. Prefer a few high-confidence findings over a long speculative list. If you find nothing new, return an empty findings array and list exactly what you attacked in stress_tested_clean.`

const SCRATCH = '/tmp/claude-0/-home-user-DB25-Semantic-Analyzer/12265417-efc3-5183-bc33-f76593ee1902/scratchpad'

const LAYERS = [
  {
    key: 'parser',
    prompt: `${COMMON}

YOUR STAGE: **db25-sql-parser** (C++23, -fno-exceptions, recursive-descent + Pratt). It bundles the tokenizer as a submodule.
  INPUT  contract (from tokenizer): the token stream.
  OUTPUT contract (to analyzer):    an ast::ASTNode tree - node types, child structure, primary_text, source ranges. This is what the analyzer reads; do NOT propose changing the AST shape to dodge a bug (that is an output-contract negotiation with the analyzer), and do NOT propose parser changes to satisfy something the analyzer wants unless the AST it currently emits is itself wrong.
Clone https://github.com/space-rf-org/db25-sql-parser.git into ${SCRATCH}/audit_parser, \`git submodule update --init --recursive\`, build Debug (cmake), run ctest. API: \`db25::parser::Parser p; auto r = p.parse(sv);\` -> ast::ASTNode*.

ATTACK (all WITHIN the parser boundary): depth-guard completeness on EVERY recursive parse_* entry (drive ~100k-500k deep, expect graceful "Maximum recursion depth exceeded", never a crash); operator precedence/associativity across the full matrix; silent mis-parses (a clause/keyword consumed wrong or dropped while the parse still succeeds -> wrong AST); memory safety via a large ASan/UBSan fuzz of malformed/truncated/random-byte/unbalanced inputs; numeric-literal edges; parse-TIME complexity (super-linear blowup on legal-but-pathological input is a DoS - measure it). Regression-audit the latest commits.`,
  },
  {
    key: 'analyzer',
    prompt: `${COMMON}

YOUR STAGE: **DB25-Semantic-Analyzer** (C++23, -fno-exceptions; name resolution/scopes, type inference & coercion, catalog, CHECK constant eval, aggregate/grouping validation, set-op reconciliation, nullability).
  INPUT  contract (from parser): the ast::ASTNode tree. If a defect is really that the parser handed you a wrong/mis-shaped AST, that is boundary="input-contract" (counterparty=parser) - do NOT compensate inside the analyzer.
  OUTPUT contract (to binder): the SAME AST annotated with context.analysis (table_id, column_id, type, nullability) + diagnostics(). The binder consumes these exact fields. If a fix would change the MEANING of an annotation the binder reads (e.g. what table_id encodes), that is boundary="output-contract" (counterparty=binder), not a unilateral change.
Clone https://github.com/space-rf-org/db25-semantic-analyzer.git into ${SCRATCH}/audit_analyzer, init submodules, build, ctest green. Read tests/ for the API (Analyzer(catalog).analyze(ast); diagnostics()/type_of/nullability_of; evaluate_check for CHECK). Postgres is the semantic reference.

ATTACK (WITHIN the analyzer boundary): type/nullability matrix corners (CASE/COALESCE/NULLIF/GREATEST/LEAST, mixed-numeric arithmetic incl. literal width, aggregate/window nullability, NULL propagation, IN/BETWEEN and every comparison sibling applying the SAME coercion rule); name resolution & scopes (correlated depth>=2, alias visibility per clause, self-joins with 3+ copies, CTE shadowing, USING/NATURAL coalescing, qualified-vs-bare refs in every clause); aggregate/grouping legality; set-op arity/type/nullability reconciliation across 3+ branches; CHECK three-valued logic and integer overflow/UB; recursion bounding. Regression-audit the latest commits.`,
  },
  {
    key: 'logical-plan',
    prompt: `${COMMON}

YOUR STAGE: **db25-logical-plan** (C++23, -fno-exceptions; binder AST->plan with numbered slots + optimizer). It pins parser + analyzer as submodules but you OWN only the binder/optimizer code (src/binder.cpp, src/expr_lower.cpp, src/optimizer*, include/db25/plan/**).
  INPUT  contract (from analyzer): the annotated AST (context.analysis: table_id, column_id, type, nullability) + catalog. If a defect is that the analyzer annotated a node WRONG, that is boundary="input-contract" (counterparty=analyzer) - do NOT fix it by second-guessing the annotation inside the binder.
  OUTPUT contract (to executor): a LogicalPlan of numbered-slot nodes. The 'hidden' ColumnSchema flag, group_keys++aggregates output model, and slot numbering are part of this contract - keep them internally consistent.
Clone https://github.com/space-rf-org/db25-logical-plan.git into ${SCRATCH}/audit_lp, init submodules, build, ctest green (3 suites). API: parse->analyze->Binder(analyzer,cat).bind(ast)->BindResult; optimize(std::move(root)); dump_plan/dump_expr.

INVARIANTS to attack (WITHIN the binder/optimizer boundary): (i) every optimizer expression walker must traverse EVERY sub-expression holding a slot ref (children, window PARTITION/ORDER BY, aggregate FILTER, subquery boundaries) - a missed branch => stale/OOB slot; (ii) optimize(optimize(p)) must equal optimize(p) structurally. ATTACK: wrong slot binding across joins/self-joins/USING/NATURAL (incl. CHAINED merges)/correlated-subqueries/CTEs; aggregate resolution by identity vs name collisions with base columns; result-changing rewrites (predicate pushdown below an outer join's null-supplying side, NOT IN with NULLs, dead-conjunct elimination, column pruning dropping a still-referenced slot); non-idempotence on stacked/nested plans; recursion bounding. Regression-audit the latest commits.`,
  },
  {
    key: 'contract-seam',
    prompt: `${COMMON}

YOUR STAGE: **the SEAMS between stages** - you do NOT own any single repo's internals; you audit whether each stage's OUTPUT actually satisfies the next stage's INPUT expectation. db25-logical-plan is the integration point (it pins parser + analyzer and drives the whole pipeline). Clone https://github.com/space-rf-org/db25-logical-plan.git into ${SCRATCH}/audit_xlayer, init submodules, build, ctest green. Write an end-to-end driver (model it on tests/test_binder.cpp) that runs a SQL string through parse -> analyze -> bind -> optimize -> optimize(optimize) under ASan/UBSan.

Your findings are CONTRACT findings by nature: for each, decide WHICH side of the seam is wrong per the contract and set boundary + counterparty_stage accordingly. Two shapes:
  - Upstream emits something that violates what downstream is entitled to assume (e.g. the analyzer marks a node resolved/typed in a way the binder then binds to the wrong slot): the DEFECT is upstream's output -> report against the upstream stage; the fix must keep downstream's input contract intact.
  - Downstream assumes MORE than the contract guarantees (it reads an incidental property upstream never promised): the defect is downstream's assumption -> report against the downstream stage.
Do NOT propose a fix that changes BOTH sides at once, and do NOT propose that a stage silently absorb the other's bug - name the single side whose contract is violated.

ATTACK THE SEAMS: an input the parser accepts but whose AST the analyzer reads under a wrong assumption; something the analyzer marks resolved/typed that the binder binds to the wrong slot or fails to bind (analyzer<->binder disagreement); a node shape reachable only through the REAL parser that the binder/optimizer mishandles; end-to-end non-idempotence; a pin bump that silently changed a contract. Drive a broad corpus (all join kinds, set-ops incl. leading-paren/nested, correlated subqueries depth>=2, GROUP BY/HAVING/window/FILTER, VALUES, DML+subqueries, CTEs, DDL) plus malformed inputs to prove the whole pipeline degrades gracefully. Regression-audit the latest commits across all three pinned stages.`,
  },
]

log(`frontend-audit: launching ${LAYERS.length} boundary-scoped agents (one per stage + contract seam) in parallel`)

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

// Flatten, tag each finding with its owning stage, and rank by severity for the
// main loop to verify + fix. `boundary`/`counterparty_stage` tell the main loop
// whether a finding is an in-stage fix or an inter-stage contract negotiation.
const SEV = { 'crash-or-ub': 0, 'wrong-result': 1, 'wrong-plan-or-slot': 2, 'non-idempotence': 3, 'minor': 4 }
const findings = results
  .filter(Boolean)
  .flatMap((r) => (r.findings || []).map((f) => ({ ...f, layer: r.layer })))
  .sort((a, b) => (SEV[a.severity] ?? 9) - (SEV[b.severity] ?? 9))

const contractFindings = findings.filter((f) => f.boundary && f.boundary !== 'internal')
log(`frontend-audit: ${findings.length} reproduced finding(s) across ${results.filter(Boolean).length} stages; ${contractFindings.length} are cross-boundary CONTRACT findings to negotiate, the rest are in-stage`)

return {
  findings,
  contract_findings: contractFindings,
  clean: results.filter(Boolean).map((r) => ({ layer: r.layer, stress_tested_clean: r.stress_tested_clean || [] })),
}
