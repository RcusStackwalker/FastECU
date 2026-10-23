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

- **Open, checks pending**: wait for them with the GitHub CLI instead of
  polling on wakeups: `gh pr checks <n> --watch --fail-fast` (run it in the
  background; CI takes 12-15 minutes and the mutation shards are the long
  pole). Do step 7 (prepare ahead) while it runs. Exit 0 means every check is
  green, 1 means one failed, 8 means still pending.
- **Open, checks green, not merged**: merge it, see step 6. If the merge is
  refused (permission denied, a required review), say so and hold; do not look
  for another way to merge it.
- **Open, `nloc-loop verdict` red**: read the failure. Fix it on the branch
  only when the guard caught a mistake in the refactor rather than a weaker
  test; otherwise close the PR with a comment naming the guard, delete the
  branch, and `nl ledger record <cluster> failed --reason <guard> --pr <n>`.
- **Merged or closed** (`gh pr view <n> --json state,mergedAt`) and the ledger
  still says `open`: record `merged` or `closed`. A prepared-ahead branch (step 7) is rebased onto the new `master`
  after a merge, and deleted after a close without merge, with its cluster
  recorded `closed`.

Done when no loop PR is open. When a PR merges, go straight on to step 2 in
the same iteration. Stop the loop (`ScheduleWakeup` with `stop`)
when the ledger shows three consecutive `failed` attempts (`unsuitable` ones
neither count nor break the run: they cost no work).

## 2. Measure the base

On an up-to-date, clean `master` (`git checkout master && git pull`): `nl measure` (keep the total as **before**)
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
`nl ledger record <cluster> unsuitable --reason "<why>"`; take the
next. Mutation cost: the tests in a cluster's scope all run in full, on both the
base and the head side, in CI. `bazel/mutation-schedule.json` holds each
test's seconds; a cluster touching a test over about 60 s (the Denso CAN
executor tests are ~60% of the total) says so in the PR body, and shrinking
that test's work is a better first cut than adding more helpers around it.

Prefer a cluster in a test that runs in well under a second, or in the shared
helper headers already in `testing/`: extend those (for example
`src/backend/flash/ecu/testing/can_executor_run.h`) before adding a local
helper per file, because per-file helpers cost about what they save. When the
top cluster alone cannot reach 50, take the sibling clusters in the same files
(for example the failure-path tests that end `plan; run; EXPECT IsErr;
EXPECT ScriptConsumed`), not another file.

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

Pitfalls seen while doing this:

- A fixture whose members are used by name must be a `struct` with public
  members: clang-tidy's `ProtectedMemberSuffix` requires a trailing `_` on
  protected members. Moving tests to `TEST_F` changes their gtest suite name,
  which is fine (the guard counts cases) but belongs in the PR body.
- A shared helper that asserts must return `void`; hand the caller what it
  needs through an `inspect` callback instead of a return value.
- Define helpers after the types they use in the shared header
  (`ExecutorRun` first), and qualify matchers (`fastecu::testing::IsOk`).
- Keep `BUILD.bazel` edits inside `testing/` directories; any other BUILD edit
  makes `production unchanged` fail. Drop and re-plan instead of arguing.
- `clang-format` runs in `prek` and can add lines back: take the NLOC
  measurement after `prek`, and leave a margin (aim for 60+ before it).
- Script edits with Python, not `sed -i` (BSD `sed -i` takes the next argument
  as a suffix). Under zsh quote globs, and do not echo `=====` separators.

Done when the focused tests pass and the change is committed. Run the Bazel
commands with explicit target labels; a shell variable holding a
newline-separated list is one invalid label under zsh.

## 5. Prove it

Every check green, or the attempt is rejected:

1. `nl measure`: total at least 50 below **before**.
2. `nl guards snapshot`, then `nl guards check --kind <kind>`. A production
   PR passes when no line it leaves alone lost coverage and the uncovered
   executable lines it adds (shared files included) do not outnumber the
   uncovered lines it deletes: moving code is free, but code a PR adds needs
   tests of its own, and the shared file's tests count toward the NLOC delta.
   Estimate that test cost before branching; a cluster whose moved code is
   mostly uncovered error branches is cheap, one that is mostly covered happy
   path leaves the tests to the new file.
3. `prek run --all-files` (then re-measure: it reformats), `python3 scripts/gazelle_check.py --fix`, and
   `PATH=/opt/homebrew/opt/llvm/bin:$PATH bazel run --config=release //:clang_tidy_report_changed`.
   Commit what they change before pushing: a pre-commit hook that reformats
   makes the first `git commit` fail, so `git add -A` and commit again.
   clang-tidy ends in `0 inline findings` when clean; grep its output for
   `error`, because `warnings treated as errors` can hide above a quiet tail.
4. `bazel test` of the touched test targets and their reverse dependencies
   (`rdeps`). The full `bazel test
   --config=release //...` is CI's job (Bazel on three OSes), not a local gate.

A red check that the refactor cannot fix without weakening a test: delete the
branch, record `failed` with the check's name, and return to step 3.

`guards check` `covered lines` failing only on files outside the diff and its
tests' dependencies is not the refactor's fault, but do not skip it silently:
name the files in the PR body (so far
`portable_cdbg_logging_protocol.cpp:72-74` and
`mitsu_colt_can_cdbg_driver.cpp:149-151`).
<!-- TODO(nloc-skill): triage rule pending the flake investigation: compare
which test binaries wrote profiles in two coverage runs of one commit
(guards snapshot runs bazel coverage; test results are cached, so a
profile-losing crash now shows as a failed test, not a missing profile). Seen on #699 and #706 at
portable_cdbg_logging_protocol.cpp:72-74 and mitsu_colt_can_cdbg_driver.cpp:149-151. -->

## 6. Open the loop PR

Push the branch and open a PR labelled `nloc-loop`, titled
`refactor(test): …` or `refactor: …` by kind. The body carries NLOC before,
after and delta (total and production/test split), the kind, the cluster ID and
the occurrences absorbed, the abstraction and why it is sound, and the
`guards check` output. Then `nl trend --pr <n> --kind <kind> --before <before>`,
commit the trend row to the branch (the hook usually fails the first commit;
add and commit again), push, and `nl ledger record <cluster> open --pr <n>`.
Write the body to a scratchpad file and use `gh pr create --label nloc-loop
--title … --body-file …`. A guard failure in CI that is not in the refactor's
files (for example a Windows-only failure in the NLOC tooling's own Python
tests) is named in the PR body, not worked around.

Merge by default. Once the trend row is pushed, wait for the checks with
`gh pr checks <n> --watch --fail-fast` and, when they are green, run
`gh pr merge <n> --auto --squash`. Waiting first matters: `master` has no
branch protection, so `--auto` merges at once instead of after CI. Then
`nl ledger record <cluster> merged --pr <n>` and carry on with step 2. If the
permission classifier denies `gh pr merge`, report it once, leave the PR open,
record nothing as merged, and wait for the user to merge it or to add a
permission rule for `gh pr merge`; do not retry in another form.

## 7. Prepare ahead

While the loop PR is in CI or review, run steps 2-4 for the next cluster on a
local branch off the open PR's branch, and stop there: open nothing until the
previous PR has merged, because the guards compare against the merge base and
the clusters usually share files. After the merge, rebase the branch onto
`master`, take a fresh `nl measure` and `nl guards snapshot`, redo step 5 and
open the PR. The merge in step 6 follows green checks, so this step is for
the wait.
