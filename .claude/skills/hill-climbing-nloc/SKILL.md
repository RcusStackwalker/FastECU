---
name: hill-climbing-nloc
description: Run one iteration of this repo's NLOC hill-climbing loop (settle the open nloc-loop PR or open the next one that lowers NLOC by 50+). Use only when the user invokes /hill-climbing-nloc or /loop /hill-climbing-nloc, or when a /loop firing carries that prompt; never for ordinary refactors, cleanup, duplicate-code questions, or NLOC measurement.
---

# NLOC hill-climbing loop

One firing is one iteration: settle the open **loop PR**, or produce the next
one. Every loop PR lowers NLOC by at least 50 without weakening what the
tests prove, and aims for 150-250 so one CI round pays for more. The rules and
their reasons are in
[ADR 0023](../../../docs/adr/0023-hill-climb-nloc-with-kind-specific-guards.md);
the commands and terms are in
[tooling](../../../docs/tooling.md#nloc-hill-climbing-loop). `nl` below means
`python3 scripts/nloc_loop.py`.

## 1. Settle the open loop PR

`gh pr list --label nloc-loop --state all --limit 5` and `nl ledger list`.

- **Open, checks pending or awaiting review**: do not idle. Do step 7
  (prepare ahead), then end the iteration; under `/loop`, wake in about 15
  minutes and look at `gh pr checks <n>` first. CI takes 12-15 minutes, and
  the mutation shards are the long pole.
- **Open, `nloc-loop verdict` red**: read the failure. Fix it on the branch
  only when the guard caught a mistake in the refactor rather than a weaker
  test; otherwise close the PR with a comment naming the guard, delete the
  branch, and `nl ledger record <cluster> rejected --reason <guard> --pr <n>`.
- **Merged or closed** and the ledger still says `open`: record `merged` or
  `closed`. A prepared-ahead branch (step 7) is rebased onto the new `master`
  after a merge, and deleted after a close without merge, with its cluster
  recorded `closed`.

Done when no loop PR is open. Stop the loop (`ScheduleWakeup` with `stop`)
when the ledger shows three consecutive rejections.

## 2. Measure the base

On an up-to-date, clean `master`: `nl measure` (keep the total as **before**)
and `nl guards snapshot` unless `.nloc-loop/snapshots/<HEAD>.json` exists.
Done when both exist for the current `master` commit.

## 3. Choose the cluster

`nl candidates`. Stop the loop when it prints nothing. Read every occurrence of
the top cluster before deciding: clusters are joined transitively, so some
occurrences are only similar. Check it against:

- the duplication items in [technical debt](../../../docs/tech-debt.md); a
  match is a reason to prefer it and the item's wording constrains the cut;
- for production code, [share only proven protocol equivalence](../../../docs/design-notes.md#share-only-proven-protocol-equivalence);
- [test structure](../../../docs/coding-style.md#structure): a shared helper is
  a `testonly` library in the owning package's `testing/` subpackage, and
  platform-specific tests stay in their own sources.

A cluster that cannot become a sound abstraction is
`nl ledger record <cluster> rejected --reason "unsuitable: <why>"`; take the
next. Mutation cost: the tests in a cluster's scope all run in full, on both the
base and the head side, in CI. `bazel/mutation-schedule.json` holds each
test's seconds; a cluster touching a test over about 60 s (the Denso CAN
executor tests are ~60% of the total) says so in the PR body, and shrinking
that test's work is a better first cut than adding more helpers around it.

Done when one cluster has a named abstraction (fixture, helper,
`TEST_P` table, builder) and the subset of occurrences it will absorb.

## 4. Refactor

Branch `refactor/nloc-<cluster>` from `master`. The kind follows the files:
touch production code only when the cluster is a production cluster. Keep
every test case: parameterize rather than merge, and keep each assertion's
meaning. Migrate occurrences until NLOC drops by 150-250, within about 1,000
changed lines (`git diff --shortstat master`), taking further clusters from
the same files when the first is exhausted. Stop earlier when the next cluster
is not a sound abstraction; 50 stays the minimum. A larger cluster continues in
the next loop PR.

Done when the focused tests pass and the change is committed. Run the Bazel
commands with explicit target labels; a shell variable holding a
newline-separated list is one invalid label under zsh.

## 5. Prove it

Every check green, or the attempt is rejected:

1. `nl measure`: total at least 50 below **before**.
2. `nl guards snapshot`, then `nl guards check --kind <kind>`.
3. `prek run --all-files`, `python3 scripts/gazelle_check.py --fix`, and
   `PATH=/opt/homebrew/opt/llvm/bin:$PATH bazel run --config=release //:clang_tidy_report_changed`.
   Commit what they change before pushing: a pre-commit hook that reformats
   makes the first `git commit` fail, so re-add and commit again.
4. `bazel test` of the touched test targets and their reverse dependencies
   (`rdeps`). The full `bazel test
   --config=release //...` is CI's job (Bazel on three OSes), not a local gate.

A red check that the refactor cannot fix without weakening a test: delete the
branch, record `rejected` with the check's name, and return to step 3.

`guards check` `covered lines` failing only on files outside the diff and its
tests' dependencies is not the refactor's fault, but do not skip it silently.
<!-- TODO(nloc-skill): triage rule pending the flake investigation: compare
which test binaries wrote profiles in two coverage runs of one commit
(coverage-local.sh runs bazel test //... with --nocache_test_results; a test
that crashes or is killed loses its profile). Seen on #699 and #706 at
portable_cdbg_logging_protocol.cpp:72-74 and mitsu_colt_can_cdbg_driver.cpp:149-151. -->

## 6. Open the loop PR

Push the branch and open a PR labelled `nloc-loop`, titled
`refactor(test): …` or `refactor: …` by kind. The body carries NLOC before,
after and delta (total and production/test split), the kind, the cluster ID and
the occurrences absorbed, the abstraction and why it is sound, and the
`guards check` output. Then `nl trend --pr <n> --kind <kind> --before <before>`,
commit the trend row to the branch, push, and
`nl ledger record <cluster> open --pr <n>`. A guard failure in CI that is not
in the refactor's files (for example a Windows-only failure in the NLOC
tooling's own Python tests) is named in the PR body, not worked around.

## 7. Prepare ahead

While the loop PR is in CI or review, run steps 2-4 for the next cluster on a
local branch off the open PR's branch, and stop there: open nothing until the
previous PR has merged, because the guards compare against the merge base and
the clusters usually share files. After the merge, rebase the branch onto
`master`, take a fresh `nl measure` and `nl guards snapshot`, redo step 5 and
open the PR. End the iteration; the CI verdict and human review decide the
open one.
