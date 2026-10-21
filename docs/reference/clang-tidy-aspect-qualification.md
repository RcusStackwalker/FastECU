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

These scoped measurements do not establish full-tree or Windows qualification.
The full source/configuration and clean/seeded finding comparisons will be
recorded from the candidate's qualification jobs and local macOS run.

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
