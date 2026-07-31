# PlayerBots — Codex Autonomous Execution Protocol

## Purpose

Codex should own the complete engineering loop for one bounded PlayerBots milestone at a time:

1. inspect;
2. plan;
3. create or select the correct branch;
4. implement;
5. add tests;
6. build;
7. run tests;
8. diagnose and fix failures;
9. repeat validation;
10. perform a final read-only review;
11. update roadmap evidence;
12. commit;
13. push;
14. write a complete handoff report.

Codex must stop at a milestone boundary. It must not silently expand from one milestone into the next.

## Autonomy boundary

Codex is authorized to perform these in-scope actions without asking again:

- read repository files and logs;
- inspect Git history and branch state;
- create a feature branch from the agreed base;
- edit milestone-scoped source, tests, build files and documentation;
- create test fixtures;
- run builds and non-destructive tests;
- fix failures caused by its changes;
- run `git diff --check`;
- perform read-only review of the complete diff;
- update milestone evidence in the roadmap;
- stage the reviewed milestone files;
- commit with a descriptive conventional commit message;
- push the milestone branch;
- write reports and patches outside the repository.

Codex is not authorized to:

- force-push;
- rewrite published history;
- reset or discard unrelated changes;
- delete databases or non-test data;
- modify secrets;
- merge branches;
- open or merge a pull request unless explicitly requested;
- continue into the next milestone after the active milestone is complete;
- claim a blocked test passed.

## Preconditions

Before changing code, Codex must verify:

- repository path;
- current branch;
- current HEAD;
- clean or intentionally understood working tree;
- milestone base commit;
- planning documents exist;
- test environment file exists when database tests are required;
- database name passes the repository's disposable-test-database guard.

If the working tree contains unrelated changes, Codex must not reset them. It must report BLOCKED unless those changes are explicitly part of the milestone.

## Milestone execution phases

### Phase A — Read and scope

Read:

- `docs/playerbots/PLAYERBOTS_VISION.md`
- `docs/playerbots/PLAYERBOTS_ROADMAP.md`
- `docs/playerbots/CODEX_REPORTING_STANDARD.md`
- relevant `AGENTS.md`
- M0 implementation and tests
- source areas required by the active milestone

Write the active milestone, exact issue-sized scope, acceptance criteria and non-goals into the report draft before editing code.

### Phase B — Repository analysis

Trace existing Canary behavior with exact file and line references.

Classify every planned change as:

- reuse existing Canary API;
- thin PlayerBot adapter;
- new general Canary abstraction;
- test-only fixture;
- deferred work.

Prefer ordinary gameplay APIs. Do not directly mutate progression or bypass validation.

### Phase C — Implementation

Implement the smallest coherent milestone slice.

Rules:

- preserve M0 lifecycle and pending-save semantics;
- keep lifecycle and gameplay logic separated;
- avoid a monolithic `BotController`;
- use stable identifiers instead of long-lived world pointers;
- use bounded tick/action execution;
- return structured results;
- make retry and terminal states explicit;
- preserve ordinary player behavior.

### Phase D — Tests

Codex must add or update tests in the same task.

Minimum test categories when relevant:

- unit tests for deterministic policies and state transitions;
- database-backed integration tests for real Player/Game/tile/persistence behavior;
- ordinary-player regression tests;
- spectator or multi-player tests;
- failure injection;
- retry and exception behavior;
- cleanup and database isolation.

Codex must list every test by name in the report.

### Phase E — Validation loop

Run the canonical gate:

```bash
"$PLAYERBOTS_ROOT/tools/run-playerbots-gate.sh"
```

When milestone-specific filters are needed:

```bash
UNIT_FILTER='<filter>' \
INTEGRATION_FILTER='<filter>' \
"$PLAYERBOTS_ROOT/tools/run-playerbots-gate.sh"
```

Codex must fix in-scope failures and rerun the complete gate.

It may not commit while any required return code is nonzero.

### Phase F — Final read-only review

After tests pass, Codex must review the complete tracked and untracked milestone diff.

Review priorities:

1. definite correctness defects;
2. lifecycle and ownership;
3. ordinary-player regressions;
4. stale world references;
5. unbounded loops or action spam;
6. persistence and retry;
7. test isolation;
8. build-system integration;
9. scope creep.

If a blocker is found:

- fix it;
- rerun the complete gate;
- repeat the review.

### Phase G — Milestone evidence

Update the roadmap only for acceptance criteria proven by code and test evidence.

Do not mark the full milestone complete unless all common Definition of Done items are satisfied.

Evidence should include:

- commit SHA;
- test counts;
- report archive path;
- known gaps.

### Phase H — Commit and push

Only after all required validation and review are green:

```bash
git add <explicit milestone files>
git diff --cached --check
git diff --cached --stat
git commit -m "<conventional milestone message>"
git push -u origin <branch>
```

Never use `git add -A` without first proving that every changed file is in scope.

After push, verify:

```bash
git status --short --branch
git log -1 --oneline --decorate
```

### Phase I — Reporting

Write the complete report according to `CODEX_REPORTING_STANDARD.md`.

Publish atomically with:

```bash
"$PLAYERBOTS_ROOT/tools/publish-codex-report.sh" \
  <draft-report-path> \
  <task-slug>
```

The report must distinguish:

- implementation status;
- build status;
- unit status;
- integration status;
- review status;
- commit status;
- push status;
- remaining validation gaps.

## Stop conditions

Codex must stop and report `BLOCKED` when:

- the repository base is wrong;
- unrelated working-tree changes cannot be safely separated;
- required secrets or test configuration are absent;
- a required database test cannot run because sandbox/network access is unavailable;
- a required test still fails after reasonable in-scope diagnosis;
- a blocker cannot be fixed without expanding scope materially;
- commit or push fails.

Codex must not convert a BLOCKED state into PASS by omitting the test.

## Database integration tests and sandbox access

The integration binary requires access to the configured MySQL/MariaDB test service.

If the Codex execution sandbox blocks local TCP or socket creation, the integration test is BLOCKED. The report must include the exact command and failure.

To let Codex perform the entire test gate itself, launch Codex in an approved environment that permits the test database connection. Do this only on the dedicated development host and disposable test database.

## Milestone boundary

At the end of a successful run, Codex must stop after:

- current milestone code is committed and pushed;
- roadmap evidence is updated;
- the final report is published.

The compact handoff must name exactly one recommended next milestone or issue-sized task.
