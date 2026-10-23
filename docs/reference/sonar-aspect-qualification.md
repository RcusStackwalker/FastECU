# Sonar aspect qualification

Status: unqualified. The [approved design](../superpowers/specs/2026-10-22-sonar-aspect-design.md)
and [implementation plan](../superpowers/plans/2026-10-22-sonar-aspect-migration.md)
define the required evidence. The existing blocking
[Sonar workflow](../../.github/workflows/sonar.yml) remains the production path.
The [temporary candidate](../../.github/workflows/sonar-qualification.yml) provides
separate coverage and scanner runners for qualification.

## Recorded local evidence

Implementation commits `047fc992`, `8b8f0803` and `1bb8d41a` provide configured
action extraction, the public preparation command and coverage artifact handling.
Focused package and scope tests passed on macOS arm64. The real configured
fixture retains both configurations through a forwarding edge and materializes
generated sources/headers while its deliberately uncompilable C++ probe remains
uncompiled. Inspection showed that action inputs omit headers deferred to
include discovery; the aspect explicitly requests the compilation header closure.
All seven focused suites subsequently passed on Ubuntu 26.04 aarch64 with
Clang/LLVM 22, including the real configured-graph regression. This narrow
Linux result does not resolve the full Qt graph limitation below.

The initial full macOS command emitted 596 configured entries, including 50
generated implementation files. All Apple SDK placeholders resolved. An
unchanged second invocation took 2.30 seconds wall time and requested no new
application compilation. This single local sample predates the retained Hedron
feature overrides and does not establish timing acceptance or Linux parity.

Coverage tests relocate reports across different temporary checkout roots,
preserve source text/external headers, reject 99 resolvable project sections,
accept 100, and reject commit/run/configuration/toolchain mismatches, corrupt
digests and malformed manifests. Producer attempt is provenance: repeat imports
do not require a scanner attempt to equal it.

## Environment limitations and remaining evidence

The local Linux probe uses an Ubuntu 26.04 aarch64 container with Clang/LLVM 22.
After selecting its compiler and installing the repository's documented Qt
runtime libraries, full preparation reached a Qt moc generator requiring an x86
runtime loader. This is not evidence for the Ubuntu x86 CI graph. Do not relax
platform selection or claim Linux qualification from that run.

The collector compares actual configured action identities and ordered arguments
against the legacy path, then compares scanner databases. Both paths retain
Hedron's target/host `compiler_param_file` and `layering_check` extraction
overrides. Normalization is limited to execution-root path prefixes, the same
selected clang driver's path spelling, Apple SDK symlinks, canonical main
repository label spelling and the debug-only prefix maps omitted by Hedron.

The full local macOS comparison subsequently passed with 596 baseline and 596
candidate configured commands, no missing/extra identities or ordered-argument
differences, and no normalized scanner-database differences. No source,
configuration or semantic flag was discarded to obtain this result. This
establishes local parity only; it does not establish Linux or cloud behavior.

The full requested-input producer dependency audit visited 2,616 actions on
macOS and found no compile/link dependency, including tool compilation. The
audit follows the configured producer graph independently of cache hits. A
separate fixture probe used two fresh output bases against a disposable local
HTTP remote cache: the second run served nine successful downloads, retained
both configured commands and materialized the generated header. This proves
fixture cache behavior; full Linux Qt inputs still require CI qualification.

Local release verification built the application and passed 299 test suites;
20 incompatible suites were skipped. All seven focused Sonar suites passed,
Gazelle produced no changes, and the pinned LLVM 23.1.2 clang-tidy gate reported
zero findings across 546 files. These checks establish local correctness gates,
not SonarCloud import or Quality Gate acceptance.

## Original candidate timings

On macOS arm64 at implementation revision `bb5aa53d` with the input-audit and
parser-refactoring changes, repeated public commands used the same checkout,
coverage configuration and selected Apple compiler. No remote cache was
configured. Warm runs reused a live Bazel server and its populated output base;
cold runs used three distinct fresh output bases with the host repository cache
retained. Source-change runs appended a unique comment to
`src/backend/logging/logging_session.cpp`, then restored the original bytes.
Each path had one unmeasured seeding run before its five warm samples.

| Preparation | Hedron median (range), seconds | Candidate median (range), seconds |
| --- | --- | --- |
| Cold, three runs | 5.263 (5.194–6.213) | 3.457 (3.455–6.115) |
| Unchanged warm, five runs | 0.982 (0.976–1.004) | 1.405 (1.384–1.470) |
| One source changed, three runs | 0.922 (0.918–0.965) | 1.428 (1.397–1.444) |

Before candidate timing, the local warm budget was fixed at at least 10%
improvement, or the baseline's relative range if larger. That requires a warm
median at or below 0.884 seconds. **The candidate fails this local budget**:
its warm median is about 43% slower. The cold median improves, but does not
satisfy the agreed warm-improvement requirement. Profiling identifies repeated
input/BEP validation and command-fragment processing as preparation costs;
those checks cannot simply be removed to claim a speedup. The legacy workflow's
separate Qt database check is excluded from these command-only baseline samples.

Same-revision Linux and whole-workflow measurements remain outstanding. Local
timings do not authorize cutover, and the warm criterion has not been waived.

Three recent successful production Sonar jobs provide historical CI context:

| Run | Input preparation including Qt check, seconds | Whole Sonar job, seconds |
| --- | --- | --- |
| [39607665936](https://github.com/RcusStackwalker/FastECU/actions/runs/39607665936/job/119248192123) | 7 | 283 |
| [39633388753](https://github.com/RcusStackwalker/FastECU/actions/runs/39633388753/job/119337484472) | 7 | 304 |
| [39670980244](https://github.com/RcusStackwalker/FastECU/actions/runs/39670980244/job/119469317158) | 6 | 273 |

GitHub's step timestamps have one-second resolution. These runs use different
source revisions and cannot establish same-revision candidate parity or warm
performance. Before candidate execution, use their 283-second median and
31-second range to set a provisional whole-workflow ceiling of 314 seconds
(median plus the greater of 10% or the observed range). Confirm the baseline on
the candidate revision before accepting a timing result. Candidate comparison
must include both jobs, their dependency wait and artifact transfer, and exclude
temporary legacy-comparison work. No whole-workflow candidate measurement exists
yet.

## Performance follow-up

The follow-up interview accepted one canonical command:
`bazel run --config=sonar_inputs //:sonar_inputs`. The config keeps the launcher
and nested extraction options consistent without changing normal application
build features. Both original entry points discarded analysis on every call
because their outer and inner feature options differed. Neither a warm server
nor action-cache hits prevented that analysis cost.

Hedron's configured `exclude_headers="all"` bypasses header discovery and its
compiler/preprocessor calls. Its query also sets `--include_artifacts=false`.
It queries and rewrites the database each time; a persistent database cache is
not the explanation for its latency here. A fresh-output-base baseline even
warned that a generated Qt source was missing, then succeeded. Extraction-only
timings must not be presented as proof of scanner-ready inputs.

Version 2 fragments retain every configured command and ordered argument but
leave the complete input closure in the declared `sonar_inputs` output group.
The wrapper checks the group's unique local paths, including symlink targets,
and independently checks source references. It deduplicates URI references
before filesystem checks and obtains the execution root from the actual BEP
workspace event. Missing root metadata, empty groups and incomplete event
streams fail. The Qt guard stops after finding a resolvable marker; full input
validation always runs before it.

`--qualification-records` exports configured commands and the unique requested
input set for the dependency audit. This opt-in file is 2,684,427 bytes for 596
commands and 5,548 inputs, compared with the original 74,077,638-byte file's
622,416 repeated input references. Ordinary preparation does not emit it, and
invalidates an older export before work begins.

Nine alternating-order pairs on the same macOS host, after seeding both paths,
used consistently configured launchers and retained analysis. No remote cache
was enabled, and generated inputs were already materialized by the candidate.

| Warm path | Median, seconds | Range, seconds |
| --- | --- | --- |
| Hedron command extraction | 0.732 | 0.700–0.767 |
| Hedron extraction plus its existing Qt check | 0.781 | 0.748–0.815 |
| Candidate complete preparation and validation | 0.758 | 0.736–1.515 |

The candidate median is about 46% lower than its original 1.405-second result.
Its approximately 3% advantage over aligned Hedron plus the Qt check is inside
the overlapping distributions; one unexplained candidate outlier is retained.
This does not establish repeatable warm improvement over a comparable complete
production preparation path. The interview retained that acceptance requirement,
prioritized CI, and required extraction, materialization and validation costs
to be reported separately. No baseline is inflated with hypothetical work.

Low-overhead stage instrumentation used Bazel's actual Python 3.14.4, unlike the
earlier Homebrew Python 3.15 profile. A separate direct-wrapper sample took
0.463 seconds: nested Bazel input build 0.309, BEP collection/local-output
validation 0.108, fragment loading 0.014, source validation 0.003, argument
localization 0.017 and Qt marker lookup below 0.001. Apple tool lookups are
included in localization. These stage measurements omit the outer launcher and
come from a different invocation than the public-command median.

After this representation change, all 596 configured commands and normalized
scanner entries still match Hedron. The full 5,548-input producer audit still
visits 2,616 actions with no compile/link requirements. The repeated fresh-base
HTTP-cache fixture again served nine downloads on its second run and retained
the generated header and both configurations. All seven focused suites and the
full release suite pass (299 passed, 20 incompatible suites skipped).

The bounded pass stops here. Cached command aggregation would not remove the
remaining Bazel invocation or full local-availability checks by itself. Full
Linux x86, SonarCloud, scanner-ready CI timing and whole-workflow evidence remain
required before production cutover.

Before cutover, record full Linux parity, a fresh scanner's requested action
closure, downloaded generated/virtual headers from a remote cache, real coverage
import, actual effective C++ scope/findings and Quality Gate outcome, successful
scanner-only retry, repeated cold/warm/source-change timings and whole-workflow
timings against baseline budgets. Record source revision, compiler versions,
cache conditions and analyzer/profile identity with the evidence.

Effective C++ test scope (verbose candidate scan on `a6646857`): 266 C++ files
matching the test patterns (`*_test.cpp`, `testing/`, `*_main.cpp`) were logged as
`excluded by project exclusion pattern`, and no such file was analyzed as a source.
This agrees with Sonar's CFamily documentation that `sonar.tests` does not make a
file analyzable, and it disagrees with the tooling guide's description of C++
suites and fixtures as analyzed tests. The migration preserves this scope; the
guide is reconciled at cutover, not here. Database membership of a test source is
not evidence of analysis. The scanner's own sensor cache was not disabled by
`sonar.cfamily.cache.enabled=false`, so no per-file analyzed list was produced; the
exclusion log is the evidence.

## Current-code macOS re-run and like-for-like timing

On the current code (checkout-rooted database, dependency headers) the macOS run
passes: 596 baseline and 596 candidate configured commands with no missing, extra or
argument differences and no database differences; the producer-closure audit visits
2,628 actions with no application compile or link; cold preparation took about 37
seconds. The earlier macOS figures are superseded by this run.

Whole-workflow timing, excluding qualification-only steps (baseline collection,
configured-graph regression, comparison, identity retention) from the candidate:
the candidate's producer plus scanner jobs took 329 and 453 seconds on two
same-revision pairs, against 289 and 279 seconds for the existing job. A third pair
is discarded because its existing job was a rerun after a SonarCloud HTTP 502. The
candidate is slower in both valid pairs and exceeds the 314 second provisional
ceiling in one; this is two samples on shared runners and not a settled
distribution, but it does not support a performance claim. The split's measured
benefit remains the scanner-only retry boundary (3 minutes 29 seconds).

Acceptance status: parity, scope, findings, coverage and retry evidence pass; the
timing requirement is not met. Per the design, no requirement is waived, so the
production cutover, guide reconciliation and the merge-time restriction of the
qualification workflow are not performed.

## Single-job candidate

The two-job split was measured at roughly +100 seconds over the existing job on the
critical path (a second runner's setup, a second cold Bazel start, preparation and
artifact transfer; paired median +178 seconds across twelve same-revision pairs
once qualification-only steps are removed). A job that starts beside the producer
and polls for it was rejected: with one available runner it holds the runner while
the producer cannot be scheduled. The candidate is now one job. The producer's
coverage build, input preparation, import and scan run in sequence on one runner,
and the import and export both validate the manifest against the job's own
`git rev-parse HEAD`.

Retry behaviour changes accordingly. There is no separate scanner job to re-run.
A whole-run rerun leaves only its own attempt's artifacts, so reuse goes through a
cache keyed by run id: the first attempt saves its exported coverage product and a
later attempt restores it, skips the coverage build and export, and still imports
and validates it (attempt 2 of run 39906977717 took 204 seconds, qualification-only
steps included). A transient scanner failure is retried once in place before the
Quality Gate.

Attempt-1 timings of the single job against the existing job on the same revision,
with qualification-only steps (baseline collection, graph regression, comparison,
identity retention) excluded from the candidate: 245 against 301 seconds, and 257
against 273 seconds. Input preparation took 8 seconds on the warm server. Two pairs
are not a distribution and do not establish the margin; the recommended protocol is
at least six to eight paired same-revision attempt-1 runs judged by the paired
delta against a margin declared in advance, not by the 314 second figure. No
performance acceptance is claimed.
