---
name: sonar-dedup-loop
description: Run one iteration of this repo's Sonar dedup loop (settle the open sonar-dedup PR or open the next one, a right-sized abstraction that lowers NLOC by 10+). Use only when the user invokes /sonar-dedup-loop, or when a /goal or /loop names it; never for ordinary refactors, cleanup, duplicate-code questions, or NLOC measurement.
---

# Sonar dedup loop

One firing is one iteration: settle the open **loop PR** (any open PR
labelled `sonar-dedup`), or produce the next one. Each loop PR adds or widens
one **right-sized abstraction** (see `GLOSSARY.md`) and lowers first-party
NLOC by at least 10. SonarCloud nominates; NLOC measures; the
`sonar-dedup verdict` check enforces. The decision is
[ADR 0029](../../../docs/adr/0029-lower-nloc-with-sonar-nominated-right-sized-abstractions.md),
the design is the
[spec](../../../docs/superpowers/specs/2026-10-25-sonar-dedup-loop-design.md).
`dl` below means `python3 scripts/dedup_loop.py`.

Launch: `/goal run /sonar-dedup-loop iterations until it prints DONE`
(fallback `/loop /sonar-dedup-loop`). End every firing with one status line,
or with a line starting `DONE:` and the reason.

## 1. Settle the open loop PR

`gh pr list --label sonar-dedup --state open` and `dl ledger list`.

- **Checks pending**: `gh pr checks <n> --watch --fail-fast` in the
  background. Wait for every check, SonarCloud and `sonar-dedup verdict`
  included: the verdict reads SonarCloud's PR analysis.
- **All green**: `gh pr merge <n> --auto --squash`, then
  `dl ledger record <id> merged --pr <n> --members-json '<members>'`. If the
  merge is refused, report it once and hold; do not merge another way.
- **Verdict or another check red**: fix it on the branch when the refactor was
  wrong (a missed caller, a survivor a test can kill). Otherwise close the PR
  with a comment naming the check, delete the branch, and record `failed
  --reason <check>`.
- **Closed without merge** and the ledger says `open`: record `closed`.

When `dl ledger list` ends `consecutive failures: 3`, stop:
`DONE: three consecutive failures`.

## 2. Fresh nominations

If `.github/workflows/sonar-master.yml` exists, after a merge run
`gh workflow run sonar-master.yml` and wait until `dl fresh` exits 0 (1 means
not yet; 2 is a SonarCloud failure: report it and stop), then
`dl trend fill-after` (the bookkeeping rides on the next loop PR). Otherwise
master is analysed only on release: proceed on the existing analysis;
`candidates` drops blocks the checkout no longer has, and the ledger keeps
merged groups out.

While waiting, you may prepare the next PR (steps 3-6) on a local branch, but
only for a group whose files the merge did not touch. Open nothing until the
previous loop PR has merged.

## 3. Choose

`git checkout master && git pull`, `dl measure` (keep the total as
**before**), and `dl candidates`. It prints nothing when the queue is empty:
`DONE: no candidates left`. It already skips settled groups and files that
open non-loop PRs touch.

Read the top group's code at HEAD, and the other groups in the same files:
one abstraction often absorbs several. Note master's `duplicated_lines`
(**dup before**) from
`curl -s 'https://sonarcloud.io/api/measures/component?component=RcusStackwalker_FastECU&metricKeys=duplicated_lines'`.

## 4. Judge

The right-sizing test. Every new or widened abstraction passes all five:

1. Named in domain language (`GLOSSARY.md`, `src/backend/flash/GLOSSARY.md`,
   [flash contracts](../../../docs/reference/flash-contracts.md)), never
   `Helper`, `Common` or `Util`.
2. Its interface is clearly smaller than what it hides: no pass-throughs, no
   flag, enum or config-struct parameter that selects per-family behavior.
3. Every caller uses the whole interface; a parameter only one caller sets
   means the cut is in the wrong place.
4. Byte-level evidence that sharing preserves protocol behavior: wire
   transcripts, plan goldens, or existing scripted wire tests that cover the
   shared path, unchanged and passing.
5. At least two callers in the same PR.

Wire behavior stays per family: wire bytes, ordering, timeouts and delays,
retry counts, address windows, and which failures abort. Log text, error
wording, progress granularity and helper structure may be normalized.

- **You see a right-sized abstraction**: go to step 5.
- **No evidence for point 4 exists** for a family in the group:
  `dl ledger record <id> deferred --reason "no wire evidence" --members-json …`
  and return to step 3.
- **You see none**: dispatch **two** fresh subagents in parallel with
  `model: opus`. Give each only the group's current code with its enclosing
  functions, the five points above, `docs/design-notes.md` and
  `docs/reference/flash-contracts.md`, not your reasoning and not each other's
  answer. Ask: is there a right-sized abstraction, and what is it? Both say
  no: record `unsuitable` with both reasons in `--reason` and return to step
  3. Either proposes one: attempt that proposal.

Never add real code to `sonar.cpd.exclusions`.

## 5. Implement

Branch `sonar-dedup/<id>` from `master`. One abstraction, every caller
migrated. A family test that now only re-proves the shared behavior may be
deleted; write down, for each, the test that now owns what it proved. Any
surviving mutant on a line you changed or moved must be killed by a new or
strengthened test in this PR; that test counts against the NLOC drop.

Pitfalls carried over from the NLOC loop:

- A shared test helper that asserts returns `void`; hand back what callers
  need through a callback.
- `clang-format` can add lines back: measure after `prek`, and keep a margin.
- Edit files with Python or the Edit tool, not `sed -i` (BSD `sed -i` takes
  the next argument as a suffix). Under zsh, quote globs and Bazel labels.
- Run Bazel with explicit labels; a variable holding a newline-separated list
  is one invalid label under zsh.

## 6. Right-size review

Dispatch one fresh subagent with `model: opus`. Give it the diff
(`git diff master`), the five points from step 4, the codebase-design
vocabulary (deep modules, interface vs implementation, seams),
`docs/design-notes.md` and `docs/reference/flash-contracts.md`, not your
reasoning. It answers pass or fail per point. Fix a failed point, or abandon:
delete the branch and record `failed --reason right-size`.

## 7. Verify locally

1. `dl measure`: total at least 10 below **before**.
2. `prek run --all-files` (then re-measure), `python3 scripts/gazelle_check.py --fix`,
   and `PATH=/opt/homebrew/opt/llvm/bin:$PATH bazel run --config=release //:clang_tidy_report_changed`;
   grep its output for `error`. Linux GCC catches sign-compare warnings macOS
   misses: build the touched targets with `--copt=-Wsign-compare` if unsure.
3. `bazel test` of the touched test targets and their `rdeps`.

A red check the refactor cannot fix: delete the branch, record `failed` with
the check's name, return to step 3.

## 8. Open the loop PR

Commit any pending bookkeeping (`docs/reference/sonar-dedup-ledger.json`,
`docs/reference/dedup-trend.csv`) on the branch and push. Write the body to a
scratchpad file:

- the abstraction, its domain name, and why it passes each of the five points;
- the group ids it absorbs, with their blocks;
- NLOC before, after and delta; master `duplicated_lines` before;
- each deleted test and the test that now owns what it proved;
- the right-size reviewer's verdict;
- the attribution footer the session requires.

`gh pr create --label sonar-dedup --title "refactor: …" --body-file <file>`
gives the PR number `<n>`. Then `dl trend add --pr <n> --before <before>
--dup-before <dup before>` and `dl ledger record <id> open --pr <n>
--members-json '<members>'`; commit both and push. Go to step 1.

The verdict judges with `master`'s copy of `scripts/dedup_loop.py` and fails a
loop PR that changes the loop's tooling (`scripts/dedup_loop.py`,
`scripts/mutation_test.py`, `.pre-commit-config.yaml`,
`sonar-project.properties`, `mull.yml`, `.github/`). Such changes go in a
normal PR.

## Stopping

Before printing `DONE:`, if ledger or trend changes are not yet on `master`,
open one bookkeeping-only PR labelled `sonar-dedup` that changes only those two
files, and settle it as in step 1. The verdict exempts such a PR from the NLOC
rule.
