# Logging models implementation plan

Spec: [Logging models design](../specs/2026-09-28-logging-models-design.md).

1. Add LoggerModel (immutable installed definition, selection setters, support
   queries/updates, capability application and support-aware default selection).
   Add portable tests for independence, identity, limits, capability asymmetry.
   Change LoggerDefinitionService fallback argument to LoggerSelection; adapt
   legacy caller and service tests. Run logging package tests and commit 6l-1.
2. Wire model/service through desktop composition. Add desktop cache keyed by
   protocol and ID with separate parameter/switch maps. Migrate snapshot builder
   and sample updates; preserve protocol filtering, offsets and conversions.
   Migrate startup, capability parsing, chooser (item data identities), displays
   and CSV (captured protocol, empty unresolved cells). Adapt existing tests and
   add exact chooser/CSV regression assertions and mutation checks.
3. Remove logging legacy files, FileActions methods and obsolete test cases,
   targets/dependencies/Qt visibility. Update roadmap/design notes. Run focused
   desktop/connection/worker tests, repository release build/tests, tidy, prek
   and local packaging. Record unavailable platform/hardware gates. Commit 6l-2.

Review focus: selection failure semantics; no definition reinstall; CSV file
lifetime across runs; supported switches with missing bytes; unresolved display
slots; protocol identity in delayed samples. The two tasks share the model and
service interface from task 1; consumers must never mutate the installed definition.

## Execution evidence

Completed the two local slices on 2026-09-28. The final macOS release application
build and repository suite passed: 231 test targets, with 7 Windows-only targets
skipped. Changed-file clang-tidy checked 19 translation units with zero findings;
`prek run --all-files` and macOS packaging passed. The first slice's portable and
legacy-caller tests passed across 11 targets.

Synthetic identity fixtures first reproduced the legacy chooser and CSV defects.
Restoring last-item chooser resolution failed all three panel cases; choosing the
first definition protocol failed the exact CSV header assertion. Restored code
passed. A fresh reviewer found a digital presentation regression; an exact
rich-text/alignment/font test failed before restoring the old presentation and
passed afterward. Two stale example XML loader comments were deferred as minor.

Execution rulings: use the clean feature checkout without a second worktree
(concurrent checkout edits remain a risk); keep current CSV choices within the
captured protocol (legacy column changes during edits remain possible); deliver
stacked PRs to the verified public, user-owned origin (two CI workloads). Hosted
Windows/Linux/macOS CI and real ECU/TCU bench outcomes are separate gates and
must not be inferred from local success. No merge or hardware qualification is
performed by this migration.

Delivery: [6l-1 (#409)](https://github.com/RcusStackwalker/FastECU/pull/409)
and [6l-2 (#410)](https://github.com/RcusStackwalker/FastECU/pull/410) remain
drafts. Hosted CI filters PRs to `master`, so both target `master` for gate
execution. Merge 6l-1 first; the [cutover-only comparison](https://github.com/RcusStackwalker/FastECU/compare/feat/6l-1-logger-model...feat/6l-logging-models)
is the review scope for 6l-2 until the first PR merges and its normal diff narrows.
