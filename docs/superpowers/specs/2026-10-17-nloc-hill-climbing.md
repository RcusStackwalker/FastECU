# NLOC hill-climbing loop

Proposed design, agreed in a design review on 2026-10-17. This is context for
the delivery PRs below, not authorization to implement them out of order.
The enduring decision is [ADR 0023](../../adr/0023-hill-climb-nloc-with-kind-specific-guards.md);
terms are defined in [tooling](../../tooling.md#nloc-hill-climbing-loop). This
spec leaves the checkout once delivery completes.

## Goal

Reduce first-party C++ size by removing duplication and introducing shared
test abstractions, one reviewed step at a time, without weakening what the
tests verify. Each step is a pull request that lowers the metric; a step
that cannot prove its tests are no weaker is discarded.

## Metric

**NLOC** is Lizard's `Total nloc` (the version pinned in
`.pre-commit-config.yaml`, currently 1.24.1): non-blank, non-comment lines,
including file-level declarations outside function bodies. clang-format is
already a gate, so layout changes cannot move it.

- Scope: C++ under `src/`, `apps/` and `tests/`, with the prek Lizard
  excludes (`third_party`, `vendor`, `generated`, `.gitignore`d files).
  Python scripts, Bazel files and docs are out of scope.
- The optimised value is the total. The production/test split is reported,
  not optimised.
- Baseline on 2026-10-17 (`34f8e41f`): **152,607**. A full measurement takes
  about 2.5 seconds.
- No CI ratchet: feature work may raise NLOC. Each merged loop PR appends one
  row (date, PR, kind, NLOC before, NLOC after) to
  `docs/reference/nloc-trend.csv`.

## Iteration

One iteration is one **loop PR** on a feature branch, reviewed and merged by
a human; the merge is the final acceptance.

- At most one loop PR is open at a time. The loop waits with a long wakeup
  until it is merged or closed, then rebases and remeasures.
- **Strict hill-climbing.** Every loop PR lowers NLOC by at least 50. An
  abstraction ships together with enough migrated call sites to pay for
  itself; there are no net-positive setup PRs.
- At most about 1,000 changed lines (additions plus deletions). A larger
  cluster is split into consecutive PRs, each net negative.

## PR kinds

Every loop PR is exactly one kind; its title says which
(`refactor(test): …` or `refactor: …`).

- **Test-only PR**: touches only `*_test.*` files, `testing/` helpers and test
  fixtures. Production sources are byte-identical to the base.
- **Production PR**: touches production code; its tests may change with it.

## Guards

All loop PRs: `bazel test //...` passes, clang-tidy and warnings-as-errors
stay clean, and no behavior changes.

| Guard | Test-only PR | Production PR |
| --- | --- | --- |
| Mutation (Mull, Linux CI, `src/algorithms` and `src/backend`) | Killed-mutant set ⊇ base's, over the production packages the touched tests cover | No new surviving mutants on changed lines |
| Coverage (local llvm-cov, includes UI and platform) | Covered-line set over production files ⊇ base's | Line coverage % of each touched production file does not drop |
| Test cases | Count does not drop | Count does not drop |

- **Set inclusion, not scores.** A score can be held constant by trading one
  kill or covered line for another; inclusion cannot. Inclusion is only
  defined while production sources are unchanged, which is why the kinds are
  never mixed.
- **Test-case count** is the number of executed `<testcase>` elements in the
  `test.xml` files Bazel writes for `bazel test //...`. Each parameterized
  instance counts once; skipped and disabled cases do not count. All tests are
  GoogleTest (including FuzzTest properties in unit-test mode), so no source
  parsing is needed.
- The test-only mutation guard is a loop-only CI job, because the existing
  PR `mutation` job mutates changed lines and so has nothing to mutate on a
  test-only change. The production-PR survivor check is blocking for loop
  PRs only. Mutation testing stays advisory for every other PR
  ([ADR 0022](../../adr/0022-advisory-mutation-testing-with-mull.md)).
- UI and platform code are in scope, guarded by coverage and test count only;
  Mull does not mutate them.

## Candidates

- **Detection.** jscpd, pinned as a manual-stage prek hook: format `cpp`,
  `--min-lines 10 --min-tokens 100`, ignoring the `sonar.cpd.exclusions` files
  and the generated-code coverage exclusions. Sonar CPD remains a CI
  cross-check.
- **Ranking.** Estimated savings = (occurrences − 1) × the shortest
  occurrence's lines. The [technical debt](../../tech-debt.md) duplication
  items name concepts, not files, so matching a cluster to one is the skill's
  judgement, not the script's ranking.
- **Eligibility.**
  - Clusters below 50 NLOC of estimated savings are dropped.
  - Files whose owning target is incompatible with the host
    (`target_compatible_with`) are dropped, so local guards see every test
    they reason about.
  - CPD-excluded files are off-limits: the protocol catalog and DTC tables are
    explicit data.
  - Accepted ADRs are hard constraints. A new shared test helper lives in the
    owning package's `testing/` directory
    ([ADR 0008](../../adr/0008-use-package-owned-mocks.md)), and platform test
    separation ([ADR 0005](../../adr/0005-separate-platform-specific-backend-tests.md))
    is preserved.
- **Order.** Test-only PRs first, starting with the `*_executor_test.cpp`
  family in `src/backend/flash/ecu`. Production PRs become eligible once the
  best remaining test-only cluster is below 50 NLOC.

## Ledger and stopping

The **ledger** is a gitignored `.nloc-loop/ledger.json` recording each
attempted cluster and its outcome, so attempts survive across sessions and
rejected clusters are not retried. A guard failure discards the branch and
records `rejected: <guard>`.

The loop stops when no untried cluster is worth at least 50 NLOC, or after
three consecutive rejections.

## Division of work

- `scripts/nloc_loop.py` (with `nloc_loop_test.py`) owns everything
  deterministic: `measure` (total and split), `candidates` (jscpd, ranking,
  eligibility), `guards` (test-case count, covered-line comparison) and
  `ledger`.
- A project skill, `.claude/skills/hill-climbing-nloc/SKILL.md`, owns the
  judgement: choosing the abstraction, refactoring, the PR body (NLOC before,
  after and delta; kind; guard results) and the stop rules.
- The loop runs in a local Claude Code session through `/loop` in self-paced
  mode, where the Qt, Bazel and LLVM toolchain already exists; the Linux-only
  mutation guards run in PR CI.

## Delivery

1. This spec, ADR 0023 and the tooling terms.
2. `scripts/nloc_loop.py` `measure`, `ledger` and the trend log; the
   `.nloc-loop/` gitignore entry.
3. The jscpd prek hook and `candidates`.
4. `guards`: test-case count and covered-line comparison.
5. The loop-only CI mutation job: the kill-set comparison for test-only PRs
   and the blocking changed-line survivor check for production PRs.
6. The `hill-climbing-nloc` skill, then the first loop PR on the CAN
   executor tests.
