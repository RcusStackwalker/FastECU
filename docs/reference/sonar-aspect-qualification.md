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

The tooling guide's C++ test-analysis description remains disputed by Sonar's
documented CFamily handling of `sonar.tests`; the real scan must settle effective
scope before documentation is reconciled. Neither database membership nor a
passing Python scope guard proves that CFamily analyzes a test source.

## Linux x86 CI evidence

Candidate runs on `ubuntu-26.04` x86 with Clang/LLVM 22 and a BuildBuddy remote
cache. Configured-command parity passed (596 baseline and 596 candidate commands,
no missing, extra or argument differences), the 664-action requested-input audit
found no application compile or link, and preparation succeeded on a fresh runner
(31.6 seconds cold, 28.4 to 9.1 seconds with a warm Bazel cache). Hedron's own
extraction timing on the baseline runner is not reported separately.

Two defects surfaced only on the scanner and are fixed:

- Entries used the Bazel execution root as their directory, so the scanner analyzed
  `.bazel/execroot/_main/...` paths and missed its sensor cache (C++ sensor 307
  seconds). Entries now use the checkout as `directory`, with `external` and
  `bazel-out` linked into it as Hedron's refresh does (48 seconds).
- A target's `CcInfo` omits `implementation_deps`, so generated `ui_*.h` headers
  reached only through them were never materialized. Twelve UI translation units
  failed to parse (98.25% statements parsed, 401 errors). The aspect now also
  requests every dependency's headers; a fixture covers the case.

On the same revision (`d38ec3d4`) both pipelines report C/C++ analysis quality
100/100 with 0 parse errors, the same 8 violations, 84.1% coverage, 45,002 lines
to cover and 7,144 uncovered lines, and both pass the Quality Gate. C++ sensor time
was 6.1 seconds for the existing job and about 11 seconds for the candidate (single
samples; not repeatable timing evidence). Both pipelines upload to the same PR
analysis, so the later upload replaces the earlier one; the comparison above uses
the analysis history and scanner logs.

The macOS parity and 2,616-action audit figures above predate the checkout-rooted
database and the `implementation_deps` header request; macOS has not been re-run on
the current code. The Linux audit visited 664 actions.

Whole-workflow timing is not yet an improvement and fails the recorded 314 second
provisional ceiling (the candidate's figure also includes qualification-only
baseline collection and comparison that the production path would omit, so it
overstates the proposed path; a like-for-like measurement is still required): the existing job took 289 seconds
and the candidate's producer plus scanner jobs took 197 plus 211 seconds in series,
because the split repeats runner and toolchain setup. The benefit claimed is the
scanner-only retry boundary. Re-running only the scanner job of run 39785871718
(attempt 2) left the producer at its attempt-1 timestamps, downloaded the same
coverage and baseline artifact IDs, passed provenance validation and the Quality
Gate, and took 3 minutes 29 seconds. The test-classification question above,
repeated timings and the independent final review remain outstanding.
