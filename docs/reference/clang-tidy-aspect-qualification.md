# clang-tidy aspect replacement qualification

Status: qualification in progress. The compilation-database runner remains the
gate until Linux, Windows and local macOS evidence satisfies the
[approved design](../superpowers/specs/2026-10-21-clang-tidy-replacement-design.md#qualification-and-ci-transition).

## Candidate and local evidence

Implementation commits `aeb887f3`, `18480813`, `a80a2cad` and `7a4a75a1` add
strict output validation, platform host/scope preservation, the recorded timing
table and final fix verification. Local checks use the official LLVM 23.1.2
installation at `/private/tmp/fastecu-llvm-qualification/macos` on macOS arm64.
The default Homebrew LLVM 23.1.3 installation was rejected by preflight.

| Probe | Evidence |
| --- | --- |
| Tooling suites | All 10 suites passed with `bazel test --config=release //tools/clang-tidy/scripts:all //:clang_tidy_configs_test //:sonar_scope_test` |
| Repository-check fixtures | All 44 fixtures passed with `bazel run --config=release //:clang_tidy_check_fixtures` |
| Actual scoped report | Checksum scope checked 17 translation units with zero findings |
| Recorded timing table | The first profiled checksum lint build took 30.560s; the warm lint build took 0.138s and executed no lint actions. Both produced identical per-check tables for the same 17 translation units |
| Actual fix and compilation | A temporary brace violation appended to `expression.cpp` was detected and fixed; final lint passed and `//src/algorithms/expression:expression` compiled. The original source was restored in a `finally` block |
| Evidence collector | A smoke run matched the nonempty compiled and linted source/target/configuration tuple for `//src/algorithms/expression:expression` |
| Full application gates | Release application build passed; the full release test graph passed 297 suites and skipped 20 platform-incompatible suites |
| Real seeded smoke | Both paths reported the same three findings: the repository template check in the probe source and brace checks in the source and a reachable `tools/` header |
| No analysis installation | `bazel build --nobuild --config=release //...` analyzed 805 targets successfully with `FASTECU_LLVM_ROOT` pointing to a nonexistent installation after marking the analysis binary manual |

The local cache-input probe selected one portable source and one desktop source:

| Changed input | Executed lint actions |
| --- | ---: |
| Unchanged warm run | 0 |
| Portable source | 1 |
| Portable header | 1 |
| Root configuration | 2 |
| Desktop child configuration | 1 |
| Repository-check implementation | 2 |
| Copied analysis executable identity, selecting the portable source only | 1 |

Every edited input was restored and subsequent unchanged runs executed zero
lint actions. A local action cache can rerun once when restoring an older input
state; this probe does not assume a historical remote-cache entry is available.
The executable probe used a disposable copied LLVM installation and left the
selected original installation untouched.

These scoped measurements do not establish full-tree or Windows qualification.
The full source/configuration and clean/seeded finding comparisons will be
recorded from the candidate's qualification jobs and local macOS run.

The full macOS clean comparison at `cbebc3fa` matched all 546 eligible sources
and all 546 source/owner-target/configuration-checksum tuples, with zero
findings and zero differences. It used
`bazel run --config=release //tools/clang-tidy/scripts:clang_tidy_qualify -- --output <new-directory>`.
The compilation database contained 596 raw entries before first-party,
generated-source and equivalent-command filtering. This completed legacy
baseline is retained while the current aspect is rerun at `5034313b`; the
guarded reference reuse verifies unchanged compiled tuples and rejects changed
production inputs, compiler setup, platform or LLVM release. The current run
passed at `5034313b`: 546 matching source/configuration tuples, no clean
findings, and the same three seeded source/header findings in both paths.
The injected files were removed and the qualification worktree was clean.

The Linux clean comparison in
[run 39642511626](https://github.com/RcusStackwalker/FastECU/actions/runs/39642511626)
also matched 546 sources and configuration tuples with zero findings or
differences, at merge revision `bd6198d4`. The seeded collector then failed
because it attempted a real merge-base lookup in a shallow checkout. Its
controlled Git boundary now supplies that lookup along with the controlled
diff; the successful full legacy baseline is retained while the current
aspect and seed detection are rerun. The reference guard compares compiler
setup and rejects build changes beyond the Windows-only wrapper patch.

The corrected Linux qualification passed in
[run 39657969954](https://github.com/RcusStackwalker/FastECU/actions/runs/39657969954)
at merge revision `1bede808`: 546 matching clean sources/configuration tuples
and no findings, then 547 matching seeded tuples and the same three seeded
diagnostics as macOS. Its current full aspect report took 43.339s, including
cached output retrieval; the seeded report took 6.610s. The macOS rerun at
`67a9af0d` also passed both phases; its full cached report took 1.274s and
executed no lint actions.

At `5034313b`, the one-time warm comparison selected
`//src/algorithms/expression:expression` on the same macOS arm64 host with LLVM
23.1.2 and a local cache. The legacy changed-file selector received controlled
empty or one-source Git input; its measured workflow included the ordinary
selected-target build and release compilation-database refresh. The unchanged
legacy run took 3.122s and analyzed no translation units; the aspect took
0.637s, reported its cached translation unit and executed zero lint actions.
Appending a harmless comment to `expression.cpp` took 8.457s for the legacy
workflow and 5.072s for the aspect, each analyzing one translation unit; the
aspect executed one lint action. Both returned zero findings and the original
source bytes were restored. The full qualification was running concurrently,
so these single samples establish behavior and observed timing, rather than a
performance threshold.

Downloaded-output coverage used a disposable loopback-only HTTP/1.1 cache
and two fresh `--batch --output_base=<temporary-directory>` builds of the
expression target, with `--remote_download_minimal` and the wrapper's output
download regex. The second build reported 29 remote-cache actions and 12
internal actions, with 51 successful cache GETs and no uploads. Human output,
exit status and replacement YAML were all materialized; the cached human
output retained its check profile and exit status was zero. The cache and
output bases used no external credentials and no source changes.

Configured identities are source/owner-target/configuration-checksum tuples
collected from Bazel's compile and aspect action graphs. Full clean parity
precedes the seeded detection phase. The latter supplies the legacy changed-file
selector with the actual disposable probe sources as a controlled Git input,
so this migration's existing enforcement diff does not widen that probe.
Evidence distinguishes eligible scope from the legacy translation units
actually analyzed in that phase. The aspect reports the selected scope,
reusing unchanged clean outputs.

## Input and platform boundaries

The aspect declares source and transitive header inputs, the governing
configuration chain, plugin and executable tools, LLVM resource headers and
selected LLVM identity. On macOS that identity includes the selected SDK's
settings, so an SDK build change at a stable path changes the input. SDK
selection is also present in compiler arguments. Installed system libraries
and SDK files are treated as installation resources; this is not a claim that
arbitrary in-place modifications to an OS installation are tracked.

The pinned LLVM's clang-cl `/std:c++latest` reports `__cplusplus=202700L`;
its GNU driver rejects `-std=c++latest`. The aspect's Windows translation maps
that option to `-std=c++2d`, which reports the same value. The actual Windows
configured analysis still requires its host qualification run.

## Retained failure regressions

The aspect suite exercises incomplete named sets, missing/invalid exit status,
failed builds with diagnostics, digest-shaped cache filenames, Windows file
URIs, missing check/tool validation, manifest coverage, final-pass fixes,
residual findings, failing compilation and replacement paths containing spaces.
Replacement text remains unchanged and replacements outside the checkout are
rejected. These unit regressions complement the real-host evidence above.
