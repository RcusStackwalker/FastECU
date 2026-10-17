---
name: hill-climbing-nloc
description: One iteration of the NLOC hill-climbing loop; run it under /loop.
disable-model-invocation: true
---

# NLOC hill-climbing loop

One firing is one iteration: settle the open **loop PR**, or produce the next
one. Every loop PR lowers NLOC by at least 50 without weakening what the
tests prove. The rules and their reasons are in
[ADR 0023](../../../docs/adr/0023-hill-climb-nloc-with-kind-specific-guards.md);
the commands and terms are in
[tooling](../../../docs/tooling.md#nloc-hill-climbing-loop). `nl` below means
`python3 scripts/nloc_loop.py`.

## 1. Settle the open loop PR

`gh pr list --label nloc-loop --state all --limit 5` and `nl ledger list`.

- **Open, checks pending or awaiting review**: end the iteration; under
  `/loop`, wake again in about 30 minutes.
- **Open, `nloc-loop verdict` red**: read the failure. Fix it on the branch
  only when the guard caught a mistake in the refactor rather than a weaker
  test; otherwise close the PR with a comment naming the guard, delete the
  branch, and `nl ledger record <cluster> rejected --reason <guard> --pr <n>`.
- **Merged or closed** and the ledger still says `open`: record `merged` or
  `closed`.

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
next. Done when one cluster has a named abstraction (fixture, helper,
`TEST_P` table, builder) and the subset of occurrences it will absorb.

## 4. Refactor

Branch `refactor/nloc-<cluster>` from `master`. The kind follows the files:
touch production code only when the cluster is a production cluster. Keep
every test case: parameterize rather than merge, and keep each assertion's
meaning. Migrate enough occurrences that NLOC drops by at least 50, within
about 1,000 changed lines (`git diff --shortstat master`); a larger cluster
continues in the next loop PR.

Done when the focused tests pass, including under GCC's sign-compare
(`--copt=-Wsign-compare --copt=-Werror=sign-compare`), and the change is
committed.

## 5. Prove it

Every check green, or the attempt is rejected:

1. `nl measure`: total at least 50 below **before**.
2. `nl guards snapshot`, then `nl guards check --kind <kind>`.
3. `prek run --all-files`, `python3 scripts/gazelle_check.py --fix`, and
   `PATH=/opt/homebrew/opt/llvm/bin:$PATH bazel run --config=release //:clang_tidy_report_changed`.
4. `bazel test --config=release //...`.

A red check that the refactor cannot fix without weakening a test: delete the
branch, record `rejected` with the check's name, and return to step 3.

## 6. Open the loop PR

Push the branch and open a PR labelled `nloc-loop`, titled
`refactor(test): …` or `refactor: …` by kind. The body carries NLOC before,
after and delta (total and production/test split), the kind, the cluster ID and
the occurrences absorbed, the abstraction and why it is sound, and the
`guards check` output. Then `nl trend --pr <n> --kind <kind> --before <before>`,
commit the trend row to the branch, push, and
`nl ledger record <cluster> open --pr <n>`. End the iteration; the CI verdict
and human review decide it.
