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

## Local preparation timings

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
