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
