# Sonar dedup loop: design

Date: 2026-10-25. Decision record: [ADR 0029](../../adr/0029-lower-nloc-with-sonar-nominated-right-sized-abstractions.md).
Terms (**duplication group**, **right-sized abstraction**, **unsuitable group**)
are defined in [GLOSSARY.md](../../../GLOSSARY.md#build).

## Intent

Bring back the agent-driven size-reduction loop that
[ADR 0024](../../adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md)
retired, rebuilt on SonarCloud's duplication data and ADR 0024's evidence
rules. The goal is not de-duplication for its own sake: each step introduces a
**right-sized abstraction** that makes the code smaller. SonarCloud says where
to look; NLOC says whether the step paid off; a written right-sizing test says
whether the abstraction is one worth having.

This serves the [size-reduction strategy](2026-10-18-size-reduction-strategy.md):
a FastECU one person can maintain with coding agents, with all flash families
and wire behavior kept.

## Goals

- A stream of small, independently mergeable PRs, each adding or widening one
  right-sized abstraction with all its callers migrated, that lowers
  first-party NLOC.
- Duplication found by SonarCloud on production code is either removed by such
  an abstraction or recorded, with reasons, as unsuitable or deferred.
- Each merge rule is checked in CI, not taken on the agent's word.

## Non-goals

- Test-code duplication. SonarCloud's CPD does not analyse test files (all 431
  report 0 duplicated lines), and test compression stays the separate,
  contract-driven work in ADR 0024. Tests are deleted only as a consequence of
  a production abstraction (see Rules).
- A duplication-density target. The loop has no number to reach.
- Changing wire behavior. The non-wire deviation rule of the `sonar-dedup`
  series applies: log text, error wording, progress granularity and helper
  structure may be normalised across families; wire bytes, ordering,
  timeouts and delays, retry counts, address windows and which failures abort
  stay per family.

## Anti-goals (failure even if every check is green)

- Abstractions that exist to make CPD quiet: `Helper`/`Common`/`Util` units,
  pass-through wrappers, a family-selecting flag, enum or config struct, or the
  "universal configurable state machine" that
  [design notes](../../design-notes.md#share-only-proven-protocol-equivalence)
  forbid.
- Weakened evidence: a deleted test without a named owner of what it proved,
  or a golden/transcript changed to make a refactor pass.
- Real code hidden from CPD through `sonar.cpd.exclusions`.

## Starting point (2026-10-25)

Master analysis `1b2a7287`: 1,621 duplicated lines, 87 blocks, 23 files, 1.8%
of 73,238 lines. Nearly all of it is in `src/backend/flash/ecu`: the two
`subaru_denso_sh7058_can*_executor.cpp` files hold ~580 lines, followed by the
TCU CVT, Hitachi M32R and SH7055/HC16 K-Line executors and three `*_plan.cpp`
files. Outside flash, `definition_authoring_dialog.cpp` holds 42.

The old loop's pieces are still live: `.github/workflows/nloc-loop.yml`, the
`hill-climbing-nloc` skill, and `scripts/nloc_loop.py` (guards, ledger, jscpd
candidates, `measure`). Master is analysed by SonarCloud only when
`release.yml` cuts a tag (`feat`/`fix` commits); `refactor` commits are not.
PR analyses contain only the files a PR changed.

## Decisions

### Metrics

- **Nomination:** a duplication group from master's SonarCloud analysis
  (`api/duplications/show`, per file), production code only.
- **Success:** a loop PR lowers first-party NLOC (Lizard, clang-formatted tree,
  `nloc_loop.py measure`, production and tests counted together) by **at
  least 10**. There is no target size; the size follows the abstraction.
- **No new duplication:** the PR's SonarCloud analysis reports
  `new_duplicated_lines == 0`. If this proves noisy on code that was only
  moved, it is downgraded to a note in the PR body.

### The right-sizing test

Every new or widened abstraction passes all five before a PR opens:

1. Named in domain language (GLOSSARY, `src/backend/flash/GLOSSARY.md`,
   flash-contracts), never `Helper`, `Common` or `Util`.
2. Its interface is clearly smaller than what it hides: no pass-throughs, no
   flag, enum or config-struct parameter that selects per-family behavior.
3. Every caller uses the whole interface; a parameter only one caller sets
   means the cut is in the wrong place.
4. Byte-level evidence that sharing preserves protocol behavior. Accepted
   evidence: wire transcripts, plan goldens, or existing scripted wire tests
   that cover the shared path, unchanged and passing.
5. At least two callers in the same PR.

A **fresh Opus review subagent** checks the diff against these five. It gets
the diff, this checklist, the codebase-design vocabulary, `design-notes.md`
and `reference/flash-contracts.md`, not the authoring agent's reasoning. A
failure is fixed or the attempt is abandoned.

### Judging a group unsuitable

A group is recorded `unsuitable` only when **two independent Opus subagents**
both find no right-sized abstraction. Each gets only the group's occurrences
(current code, with enclosing functions), the right-sizing test,
`design-notes.md` and `reference/flash-contracts.md`, not the loop's
reasoning and not each other's answer. If either proposes an abstraction, the
loop attempts that proposal. Both reasons go into the ledger. Judging by
reading costs no implementation and does not count toward the stop rule.

A group whose families have none of the evidence in item 4 is recorded
`deferred: no wire evidence`. Deferred groups are neither unsuitable nor
failures; adding wire evidence is the guarded-transport rollout's job.

`sonar.cpd.exclusions` stays limited to explicit per-family data tables, as
today.

### The unit of work

One right-sized abstraction, all of its callers migrated, plus the tests it
makes redundant. A family test that now only re-proves the shared behavior may
be deleted in the same PR; the PR body names, for each deleted test, the test
that now owns what it proved (ADR 0024). Sibling groups in the same files that
the same abstraction absorbs go in the same PR; unrelated cuts do not.

### Ranking and selection

- Rank groups by duplicated lines, backend groups first; `src/ui` and
  `src/platform` groups after them (Mull does not mutate those; their
  evidence is changed-line coverage plus the right-sizing review).
- Read the top group and its siblings in the same files before deciding.
- Skip a group whose files are touched by an open non-loop PR (e.g. the
  guarded-transport rollout of ADR 0025).
- Skip groups the ledger records `unsuitable`, `deferred`, or `merged`.

### Freshness of the candidate list

After a loop PR merges, the loop starts a master SonarCloud analysis itself
(`gh workflow run` on a new `workflow_dispatch` entry that analyses
`master` HEAD) and chooses the next group only from an analysis whose revision
equals `origin/master` HEAD. While it waits, it may prepare the next PR from the
older list, but only for a group whose files the merge did not touch.

SonarCloud's new-code period for `master` is "previous version" (checked
2026-10-25) and `sonar.projectVersion` is unset, so these extra analyses leave
the release-time quality gate's baseline alone; the loop dispatches them
through `.github/workflows/sonar-master.yml`.

### Merge evidence

All of these, before the loop merges:

- Plan goldens and wire transcripts unchanged.
- Changed lines covered.
- No surviving Mull mutants on changed lines, as the existing `pr.yml`
  `mutation` job reports them (it mutates changed lines and does not compare
  against the base; ADR 0024's "lines a PR adds or changes"). Code moved into
  an abstraction counts as changed, so survivors it carried must be killed by
  new or strengthened tests in the same PR; that cost is part of the NLOC
  measure. This **blocks loop PRs only**; mutation testing stays advisory for
  every other PR ([ADR 0022](../../adr/0022-advisory-mutation-testing-with-mull.md)).
- NLOC drop ≥ 10 and `new_duplicated_lines == 0`.
- A red SonarCloud quality gate on new-code duplication does not by itself
  block (as in the `sonar-dedup` series today); the `new_duplicated_lines`
  rule above is the duplication check.

A **`sonar-dedup verdict`** job in `pr.yml`, run only on PRs labelled
`sonar-dedup`, replaces `nloc-loop.yml`. It `needs:` both the `sonarcloud` and
`mutation` jobs and fails on an NLOC drop under 10 (Lizard, base vs head), on
survivors reported by the `mutation` job (it reuses that job's output and does
not run Mull again), on uncovered changed lines, or on
`new_duplicated_lines > 0` in the PR's SonarCloud analysis. It is the check the
loop waits on. Because changed-line coverage and new duplication come only
from the SonarCloud job, **loop PRs wait for SonarCloud**; the old "don't wait
for SonarCloud" exception does not apply to them.

Enforcement is the agent's wait (`gh pr checks --watch`) before
`gh pr merge --auto --squash`, as today: `master` has no branch protection and
none is added. A ruleset requiring the verdict can be added later if the
verdict is made to pass on unlabelled PRs.

### Ledger and trend

- The ledger is committed, e.g. `docs/reference/sonar-dedup-ledger.json`, so
  judgments survive a fresh clone and are reviewed in PRs. Entries are keyed by
  the sorted set of `(file, enclosing function)` pairs of the group, since
  SonarCloud's block positions change between analyses. Outcomes: `open`,
  `merged`, `closed`, `failed` (with guard), `unsuitable` (with both
  reviewers' reasons), `deferred` (with reason).
- `docs/reference/nloc-trend.csv` becomes `dedup-trend.csv`: date, PR, NLOC
  before, NLOC after, master `duplicated_lines` before, after (the "after" from
  the post-merge analysis).
- Bookkeeping that has no PR of its own (unsuitable/deferred judgments, the
  previous row's "after" value) is committed on the next loop PR's branch.
  When the loop stops with bookkeeping still uncommitted, it opens one final
  `sonar-dedup` PR carrying only the ledger and trend changes; that PR is
  exempt from the NLOC rule. *(Builder-check resolution, Claude's call.)*

### Stop rule

The loop stops, printing a line that begins `DONE:` with the reason, when:

- no group remains that is not `unsuitable`, `deferred`, `merged` or skipped,
  or
- three attempts in a row ended `failed` after implementation.

### Driving the loop

The skill is driver-agnostic: one firing is one iteration (settle the open
loop PR, or produce the next one) and ends with either a status line or
`DONE: <reason>`. It is launched with `/goal`, e.g.
`/goal run /sonar-dedup-loop iterations until it prints DONE`; `/goal`'s
evaluator can see the `DONE:` line in the transcript and survives
`--resume`. CI is waited on with background `gh pr checks --watch`, not
polling. `/loop` (self-paced) remains a fallback. Cloud routines are not used:
each iteration needs a warm Bazel cache and local tooling.

The loop merges its own PR with `gh pr merge --auto --squash` after every
check, SonarCloud and the verdict included, is green; if the merge is refused, it
reports once and holds. At most one loop PR is open at a time.

### Retiring the old loop

The first PR of this work deletes `nloc-loop.yml`, the `hill-climbing-nloc`
skill, and everything in `scripts/nloc_loop.py` except `measure` (with their
tests), and adds the `sonar-dedup-loop` skill. Two loop skills must never be
listed together. `docs/tooling.md`'s "NLOC hill-climbing loop" section is
rewritten for the new loop.

The `sonar-dedup` label and its series continue as this loop's output. From
the first PR on, every `sonar-dedup` PR is a loop PR and follows these rules,
whether the loop or a person opened it; there is no second marker. *(Builder-
check resolution, Claude's call, following the decision that the loop absorbs
the series.)*

## Left to the builder

- Script layout: extend `scripts/nloc_loop.py` (renamed) or add the Sonar
  client to `tools/sonar/`; how groups are assembled from per-file
  `api/duplications/show` results.
- Mapping SonarCloud blocks to enclosing functions (Lizard's function ranges
  are one option).
- The exact `workflow_dispatch` wiring and how the verdict reads the `mutation`
  job's output and the PR's `new_duplicated_lines`.
- Whether the old `nloc-loop` GitHub label is deleted.
- Skill wording, following the structure of the retired `hill-climbing-nloc`
  skill where it still applies.
