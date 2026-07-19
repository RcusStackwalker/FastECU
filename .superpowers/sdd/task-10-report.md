# Task 10 report: desktop platform implementations

## Result

Implemented the mechanical desktop-platform relocation from base
`39810ed98fd02dd07f66f92610db9d0cdabc1854` on branch
`refactor/step2-pr10-platform`.

## Relocation evidence

- `src/platform/desktop/common/serial/`: 14 files. This is every former
  `serial_port/` file not explicitly classified as Unix or Windows, including
  `j2534_driver_selection.h`, the replica definition, QtRO helper, common
  backends/actions, and WebSocket device.
- `src/platform/desktop/unix/j2534/`: exactly 3 files:
  `J2534_unix.{h,cpp}` and `J2534_tactrix_unix.h`.
- `src/platform/desktop/windows/j2534/`: 11 files including the nested host
  package: `J2534_win.{h,cpp}`, `J2534_tactrix_win.h`, both bridge
  implementation/header pairs, `pe_bitness.{h,cpp}`, and
  `j2534_bridge_host/{BUILD.bazel,main.cpp}`.
- `src/platform/desktop/common/remote_utility/`: exactly 3 files, preserving
  the `.rep` replica source.
- `src/platform/desktop/common/transport/`: exactly the 3 requested concrete
  transport pairs (6 files).
- `src/platform/desktop/common/logging/`: exactly `systemlogger.{h,cpp}`.
- Moves were performed with `git mv`.
- `find serial_port remote_utility -type f -print 2>/dev/null` produced no
  output. The legacy directories are gone.
- The six moved concrete transport files are absent from top-level
  `protocol/`; all other protocol files remain protected and present there.

## Consumer and semantic audit

- Updated `bazel/fastecu_sources.bzl` common/Unix/Windows source, header, MOC,
  replica, and test suite lists, including the intentionally different path
  roots used by the main workspace and generated test workspace.
- Updated root BUILD sources/headers/include exports while retaining root
  target names `j2534_bridge_protocol`, `pe_bitness`, and
  `j2534_bridge_client` and their original Windows-only
  `target_compatible_with` selects.
- The nested host target names and compatibility declarations are unchanged;
  tests and Windows packaging now consume its relocated package label.
- Updated common Bazel include paths, platform/test suite macros, all C++
  includes, test data/location expressions, coverage baseline paths, Windows
  packaging commands, and the Windows preprocessor-guard test data path.
- Replica file contents and target-generation semantics are unchanged; only
  their source paths changed.
- Exhaustive tracked/untracked scan for old filesystem consumers
  (`//serial_port`, `//remote_utility`, `-Iserial_port`, old include prefixes,
  old concrete transport paths, and old logger paths) returned no stale
  references. Runtime identifiers such as the `serial_port` configuration key
  were explicitly preserved.
- `git diff --check`: passed.

## Verification

- `bazel query 'tests(//tests/...)' | rg
  '(serial|pty|remote|j2534|pe_bitness|transport|facade_threading)'`: passed and
  resolved eight focused labels (`j2534_bridge_client_test`,
  `j2534_bridge_integration_test`, `j2534_bridge_protocol_test`,
  `j2534_win_bridge_test`, `pe_bitness_test`, `serial_backend_tests`,
  `serial_crash_tests`, and `test_transport`).
- The bounded focused `bazel test --config=release` invocation did not reach
  compilation: macOS analysis rejected the explicitly requested Windows-only
  `//tests:pe_bitness_test` as incompatible. Per coordination instruction, it
  was terminated and no further Bazel commands were run. Windows-specific
  J2534/PE execution remains CI-only.
- `prek run --all-files`: passed on the final tree after buildifier's initial
  mechanical formatting pass. Passing hooks: clang-format, buildifier,
  buildifier-lint, ruff check, and ruff format.

## Concern

The local platform-focused Bazel test batch is not a passing test result
because explicitly including Windows-only labels aborts analysis on macOS.
The static query, compatibility preservation audit, stale-path scans,
formatting/lint gate, and diff checks passed; the full three-OS and packaging
matrix must validate Windows-specific compilation and tests.
