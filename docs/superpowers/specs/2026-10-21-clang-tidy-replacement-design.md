# Replace the compilation-database clang-tidy runner

Status: proposed; interview decisions recorded, awaiting confirmation of the
complete design. The [tooling guide](../../tooling.md#clang-tidy)
describes the current gate; the [aspect spike](2026-10-21-clang-tidy-aspect-spike.md)
records the original experiment.

## Purpose

Make Bazel aspect actions the sole application-analysis path for clang-tidy.
Bazel's declared inputs and action cache replace the runner's manual
changed-files selection, merge-base handling and configuration-change widening.

## Agreed requirements

The interview has settled these requirements:

- Preserve Linux analysis, Windows-exclusive analysis and local macOS support
  before deleting the old runner. A macOS CI job is not required.
- Expose `bazel run //:clang_tidy` for reports and `-- --fix` for fixes. Remove
  the legacy runner interface, implementation, exclusive tests and callers;
  do not retain compatibility aliases or a separate changed-files mode.
- Retain compilation-database generation for its independent SonarCloud
  consumer. Shared fixture and plugin support must survive deletion of the
  legacy driver.
- Qualify matching source coverage and findings, including deliberately
  introduced violations detected by both paths. Two clean runs alone are
  insufficient evidence.
- Verify fixes, annotations and action invalidation after source, header,
  configuration and plugin changes. Record warm-cache timing against the old
  runner before removal.
- Replace the built-in versus repository-check comparison benchmark with one
  table showing the time spent in each check. Remove the comparison driver and
  its exclusive tests; retain shared profiling support where useful. This does
  not remove the one-time migration timing evidence required above.
- After fixes, always re-analyze the final modified sources and run an ordinary
  build of the selected targets with the same configuration. Remaining findings
  and build failures return failure. A bounded fix loop must not apply a final
  batch without verifying it.
- Keep the existing fuzz-harness coverage gap as separate work. Preserve its
  entry in technical debt, updating its explanation for the aspect workflow.
- Collect per-check timings during normal lint actions. Aggregate the selected
  scope, including cached outputs, into one table ordered by descending time.
  Display it in CI and locally with `--profile`; requesting the table does not
  force fresh analysis. Label its metric as recorded wall seconds summed across
  translation units, rather than current-run elapsed time or CPU time.

## Replacement contract

### Analysis and platforms

Continue using the patched `aspect_rules_lint` aspect, rather than introducing
a second aspect implementation. Bazel compilation contexts provide flags,
declared headers and generated-header prerequisites. Analysis does not refresh
a compilation database or perform a full application prebuild.

Linux analyzes the full supported default scope; Windows retains the
[scope manifest](../../../.clang-tidy-scope.toml) and its uncovered
platform-package guard. Selecting Windows targets must also constrain reported
source scope: dependencies outside the manifest must not silently broaden it.
Local macOS retains SDK selection. Preserve distinct compilation configurations
of a source, generated-source exclusions and the existing first-party header
coverage, including `tools/` and `.hpp` headers.

Keep the current pinned LLVM release, artifact identity and registered-check
validation. Reuse the shared toolchain validation also used by fixtures.
Linux/macOS load the shared extension; Windows uses the analysis executable
with repository checks linked into it. Missing tools, incompatible artifacts,
missing required checks and an empty analysis scope fail explicitly.

### Commands and fixes

The public report command is `bazel run //:clang_tidy`; optional target patterns
narrow the selected graph. `--fix`, `--profile` and `--annotate` are options on
that command. Preserve release configuration as the default analysis mode and
forward requested Bazel configuration consistently to nested lint and post-fix
builds. Windows CI retains explicit Windows scope selection. Fix support remains
macOS/Linux, matching the existing supported contract.

The aspect exports replacements from sandboxed analysis. The wrapper maps them
back to checkout paths, applies them with the selected LLVM's
`clang-apply-replacements`, and re-analyzes. Fix application is bounded; the last
application always has a subsequent check. After applying fixes, an ordinary
Bazel build verifies the selected targets with the same configuration, including
when findings remain. Tool failures and unsuccessful builds cannot produce a
successful result. Residual findings remain failures even when other findings
were fixed. Repository compiler warnings remain governed by the existing
[warning policy](../../adr/0018-treat-first-party-warnings-as-errors.md).

### Reports and timing

Keep readable findings, source context, deduplication and GitHub annotations,
including findings retrieved from cache. A failed analysis without a parseable
diagnostic still fails. Cached output must be available locally for reporting;
unavailable or malformed required outputs must not silently count as success.

Pass `--enable-check-profile` to normal lint actions and retain its stderr table
in the existing declared human output. The wrapper aggregates raw timing data
before the diagnostic formatter suppresses individual profile tables. This
uses existing action outputs and needs no separate JSON-profile output format.

Render one table of reported checks, summed wall seconds and share of the
reported check total, ordered slowest first. Print it in the CI log and job
summary, and locally when `--profile` is requested. Cached samples can originate
from different runs and machines; the table is a descriptive cost breakdown,
not a benchmark or a measurement of work executed by the current command.
Missing timing data is reported explicitly, without inventing zero values.

### Cache correctness

Sources, relevant transitive headers, governing configuration chains, the
repository-check artifact and selected analysis executable must participate in
action invalidation. Preserve the configuration-chain generator and its gate;
a child configuration change must not invalidate unrelated configuration
subtrees. Remove the deleted exception-file reference.

Qualification must audit toolchain resources that are outside the executable
itself, including LLVM resource headers and SDK selection. An executable hash
alone is not evidence that all relevant installation inputs are tracked. Resolve
any uncovered input dependency before claiming cache correctness; keep the
scope of that claim explicit in the evidence.

## Qualification and CI transition

Keep the legacy runner available until the replacement evidence is recorded.
Qualification uses the same revision, platform, LLVM and build configuration
for each comparison. Record the exact revisions, commands, selected source and
configuration sets, findings, cache conditions and timings.

| Area | Required evidence |
| --- | --- |
| Linux, Windows scope and local macOS | Matching eligible source/configuration sets and explained generated/vendor exclusions; matching clean-tree findings |
| Detection | Seed built-in and repository-check violations in sources and first-party headers; confirm both paths detect them, including Windows-specific code and a `tools/` header reachable in scope |
| Configuration | Exercise inherited root and child configurations, and prove that unrelated subtrees retain cached lint actions |
| Input invalidation | Change a source, a dependent header, repository-check implementation and selected analysis tool identity; verify the appropriate actions rerun |
| Warm cache | Compare unchanged and one-source-edit runs against the legacy runner on the same machine and scope; identify executed versus cached actions |
| Fixes on macOS/Linux | Demonstrate final re-analysis and ordinary compilation after fixes, including residual findings, a fix at the loop limit and a failing post-fix build |
| Reporting | Exercise cached diagnostics, deduplication, annotations, the aggregate timing table and downloaded remote outputs |
| Failures | Reject missing checks, incorrect LLVM identity, an invalid Windows manifest, empty scope and failed or incomplete analysis outputs |

Source/configuration or finding differences must be explained and reconciled;
empty matching finding sets alone are insufficient. Timing is recorded and
reviewed evidence, without introducing a numerical CI performance threshold.
The retired built-in/custom-check comparison benchmark is not needed for this
one-time migration comparison.

The final PR replaces the legacy report steps inside the existing blocking
Linux/Windows analysis jobs, retains repository-check fixtures, and removes the
separate advisory aspect job. Before merge, both replacement platform jobs must
pass and the local macOS evidence must be recorded. There is no legacy fallback
in the delivered implementation.

Delete the old report/fix/changed/benchmark targets, runner-specific code and
tests, manual changed-file selection and parity helpers whose sole purpose was
the migration. Preserve shared report, timing, toolchain, scope and fixture
support as needed by the replacement; remove unused exports and dependencies.
Keep the compilation-database target available to SonarCloud.

## Documentation and delivery

Implement on a feature branch through a pull request. The implementation must
update the [tooling guide](../../tooling.md#clang-tidy), repository commands,
tooling READMEs and affected build comments. Update the compile-command
description in [ADR 0007](../../adr/0007-use-bazel-as-ci-source-of-truth.md)
and stale runner-specific context in
[ADR 0017](../../adr/0017-generate-bazel-targets-with-gazelle.md), preserving
their historical context. Keep the fuzz-harness gap in technical debt and
describe it in terms of the selected analysis configuration.

Record qualification evidence in a focused reference linked from the tooling
guide. This design needs no new glossary entries for general tooling concepts
or a separate ADR duplicating the existing Bazel ownership decision.

Use focused tooling tests during implementation; before delivery run the
applicable full Bazel, formatting, Gazelle, static-analysis and platform gates
required by the repository. This design-only change uses the Markdown link
checker and scoped diff review.

## Current implementation and recorded evidence

The [PR workflow](../../../.github/workflows/pr.yml) still runs the
compilation-database runner as a blocking Linux/Windows gate. Its separate
Linux aspect job is advisory.

Commit `f17f5abb` records macOS parity for the same 546 sources, clean runs and
three seeded findings. Recorded cold times are 8m31s for the old runner and
9m04s for the aspect; warm aspect times are 3s unchanged and 2s after a source
edit. The equivalent old-runner one-file measurement was not made.

Commit `c4ed2287` records Linux runs of 546 sources with zero findings on both
paths, and a 46s aspect build with 749 of 778 actions cached. Exact source-set
comparison and deliberately introduced Linux findings were not verified there.
These historical measurements do not establish qualification of the current
tree or Windows support.

The inspected implementation disagrees with parts of the spike and intended
contract:

- The aspect still names `.clang-tidy-exceptions.yaml`, removed by commit
  `59d58722`. The old exception audit is already retired.
- The aspect hardcodes the shared plugin, while Windows requires the existing
  analysis host with repository checks linked into it.
- The legacy header filter includes `tools/`; the root configuration inherited
  by the aspect instead includes `scripts/`. Preserve the existing first-party
  header coverage when reconciling this difference.
- macOS SDK arguments already exist in `bazel/llvm.bzl`, although the spike
  calls that work deferred.
- Legacy fixes on macOS/Linux run an ordinary build after applying
  replacements. The aspect currently requests only lint outputs and can apply
  replacements on its final pass without subsequently checking them.
- The legacy profiling code sums per-file wall time, although the tooling
  guide calls it CPU time. Any retained profiling must label its measurements
  correctly and distinguish cached results from freshly executed work.

## Review state

All interview questions have answers. Confirm the complete contract above
before implementation planning. Enduring usage and architecture knowledge will
move to their owning documents when the replacement is implemented. Preserve
this proposal in delivered Git history before deleting it, following the
[completed-work retention policy](../../README.md#recover-completed-work-and-deleted-source-citations).
