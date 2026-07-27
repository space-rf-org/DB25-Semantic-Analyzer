# DB25 frontend audit — runbook

`frontend-audit.js` is an **evergreen, repeatable** adversarial correctness
audit of the whole DB25 SQL frontend cascade:

```
db25-sql-tokenizer → db25-sql-parser → DB25-Semantic-Analyzer → db25-logical-plan
```

## When to run it

**After landing anything major** in any of the four layers — a new clause, a
resolver/type-inference change, a binder/optimizer rewrite, a submodule pin
bump. The cascade compounds defects upward, so a change low in the stack is
exactly when a fresh sweep pays off.

## How to run it

From a Claude Code session in this repo:

- `/workflows` → run **frontend-audit**, or
- ask Claude to `Workflow({ name: 'frontend-audit' })`.

It fans out **one adversarial agent per layer plus a cross-layer seam sweep**,
in parallel. Each agent:

1. clones its layer fresh, inits submodules, and gets the existing test suite
   **green first** (mirroring the CI sanitizer flags,
   `-fsanitize=address,undefined -fno-sanitize-recover=all`);
2. runs `git log` and **regression-audits the most recent commits** — attacking
   the newly-changed code paths specifically;
3. reports only **reproduced** defects (sanitizer trace, or actual-vs-expected
   AST/plan/diagnostic captured), ranked
   `crash-or-ub > wrong-result > wrong-plan-or-slot > non-idempotence > minor`.

The script returns `{ findings, clean }`. The main loop then independently
**re-verifies** each reproduced finding and fixes the real ones **bottom-up,
one PR per finding** — the standing DB25 discipline: strengthen the lower layer
before moving up.

## Why it stays useful (evergreen by design)

It hardcodes **no** list of "already fixed" defects — that would rot. Instead
each agent treats anything a regression test already pins as *known*, filters
out documented design choices and dialect gaps, and re-derives what's new from
`git log`. So the same script keeps finding *new* and *newly-introduced*
defects as the codebase evolves, run after run.
