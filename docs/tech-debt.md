# FastECU technical debt roadmap

This document tracks unresolved project-wide technical debt that affects
testability, legibility, and structure. Completed work is intentionally removed
instead of retained as an implementation log; use Git history and the ADRs for
that history.

The target state is:

- 80% automated test coverage for maintained, non-generated application code.
- Clear separation between UI, hardware I/O, protocol logic, and data/model code.
- Smaller units that can be tested without a real ECU, J2534 adapter, serial port,
  GUI dialog, or user home-directory configuration.
- A build/test setup that makes regressions visible in CI before bench testing.

## Current snapshot

Observed on 2026-08-05:

- FastECU is a Qt 6/C++23 desktop application. Bazel is the sole target graph for
  the application, tests, release packaging, coverage, compile commands, and
  clang-tidy; the qmake project files have been removed (ADR 0001).
- The tracked maintained C++ surface, excluding tests, `src/ui/desktop/hexedit/`,
  and generated Qt files, is approximately 390 `.cpp`/`.h` files and 93k lines.
  Tests contain approximately 24k lines across 104 `.cpp`/`.h` files.
- Tests are strongest around protocol codecs, logging, serial threading, J2534
  bridge behavior, definition parsing, and the extracted config/calibration use
  cases. Checksum families, calibration/map editing, most flash orchestration,
  and UI workflows remain lightly covered.
- CI builds and tests on Windows, macOS, and Linux, verifies macOS/Windows
  packages, produces coverage for SonarCloud, and runs clang-tidy as a
  non-blocking report.
- Focused background notes remain in the
  [logging-engine notes](logging-engine-tech-debt.md) and the
  [protocol generalization notes](protocol-generalization-opportunities.md);
  those documents contain the current logging-specific gaps and safe
  protocol-sharing boundary.

## Priorities

### P0: Make coverage results trustworthy

Coverage gating now happens once, via the SonarCloud Quality Gate: `pr.yml`
runs `scripts/coverage-local.sh`, imports `coverage/llvm-cov.report` into the
SonarCloud scan, and the `Check SonarCloud Quality Gate` step fails the job
when the gate (including the new-code coverage condition) is not `OK`. The
previously separate `scripts/check-coverage-ratchet.sh` /
`docs/coverage-baseline.txt` overall-coverage ratchet has been removed — it
duplicated this signal with a stale qmake-era baseline (25.17% line coverage)
that predated the Bazel source reorganization and didn't actually block merges.

Remaining gaps:

- The package-owned serial tests inherit an intermittent Windows-only crash
  from the former aggregate `serial_backend_tests` target. The split targets
  retain unbuffered diagnostics so the failing binary and slot can be isolated;
  until then, the crash should not be attributed to one suite or used as a
  reason to ignore unrelated coverage-test failures.

Actions:

- Resolve or explicitly quarantine the intermittent serial backend test with a
  separate visible CI result and an owner; do not silently discard its exit
  status.
- Keep exclusions explicit and reviewed: tests, generated Qt files, vendored
  `src/ui/desktop/hexedit/`, Bazel/external outputs, system libraries, and platform SDKs.

### P1: Separate UI from application logic

`MainWindow` remains the central coordinator for startup, settings/config
loading, serial/device setup, logging wiring, calibration lifecycle, ECU
operation dispatch, log views, status updates, and dialogs. `mainwindow.h`
still includes nearly every flash dialog module. `mainwindow.cpp` is about
2.7k lines and `menu_actions.cpp` is about 2k lines.

Risks:

- Most workflows require a live `QMainWindow` or `QApplication` to test.
- Adding a flash module or application workflow tends to touch central UI code
  and its large include graph.
- Calibration rules and command behavior remain mixed with widget lookup,
  message boxes, table selection, and shared state mutation.

Actions:

- Introduce small application services behind `MainWindow`: protocol selection,
  calibration sessions, logging sessions, flash-operation dispatch, and
  settings persistence.
- Replace direct construction of all flash dialogs from `MainWindow` with a
  typed operation registry/factory that owns module-specific dependencies.
- Move calibration/map commands out of `menu_actions.cpp` into headless model
  operations; leave only selection extraction, signal wiring, and user feedback
  in the UI.
- Keep new file, protocol, and hardware logic out of `MainWindow`.

### P1: Split `FileActions`

`FileActions` is still a 2.1k-line `QWidget` implementation with a 361-line
header. Expression and diagnostic parsing, the EcuFlash/RomRaider parsers, ROM
open/save, config persistence, and checksum dispatch have been extracted into
portable use cases under `src/backend/{definition,calibration,config,checksum}/`,
each reached through a `Legacy*Adapter`. What remains inside `FileActions` is
logger definition/conf reading, menu loading, the nested `ConfigValuesStructure`
/ `LogValuesStructure` / `EcuCalDefStructure` models, and direct
`QFileDialog`/`QMessageBox` behavior.

Actions:

- Extract `LoggerDefinitionParser` as a non-widget component with explicit
  inputs and results, following the definition-parser precedent.
- Make dialogs and message boxes caller-owned UI behavior.
- Move nested data structures to standalone model headers so parsers, logging,
  calibration editing, and tests do not depend on `FileActions`.
- Remove the `Legacy*Adapter` compatibility wrappers after all callers use the
  extracted APIs.

### P1: Replace parallel-list data models

Validation now catches several length mismatches after parsing, but core models
are still represented by large parallel `QStringList` collections and raw
index/pointer ownership. Examples include
`FileActions::ConfigValuesStructure`, `LogValuesStructure`, and
`EcuCalDefStructure`; `MainWindow` also owns a fixed raw-pointer array of 100
calibration definitions.

Risks:

- Every mutation must keep many lists aligned, and validation only detects an
  inconsistency after it has been created.
- UI, logging, and protocol code use integer indexes into shared mutable state.
- Tests must populate unrelated fields and cannot express model invariants in
  the type system.

Actions:

- Convert high-churn rows to typed values one area at a time, starting with
  `LoggerChannel`, `LoggerSwitch`, `ProtocolDefinition`, `CalibrationMap`, and
  `RomDefinition`.
- Make parsers construct and validate complete records instead of appending to
  parallel lists.
- Return immutable models or controlled mutation APIs after parsing.
- Replace raw fixed-capacity ownership with containers of values or smart
  pointers, and return `std::optional`/explicit result types instead of null or
  partially filled structures.

### P1: Isolate flash-operation orchestration

All 29 flash/eeprom/jtag/bdm operation pairs use `FlashOperationWorker`, and
shared SSM framing, seed/payload transforms, CRC, byte formatting, byte
stuffing, and ISO-15765 setup have been consolidated. The remaining safe
generalization opportunities are maintained in the
[protocol generalization notes](protocol-generalization-opportunities.md).

The operation classes still combine request construction, response validation,
retries, progress reporting, prompts, full-facade serial I/O, and ROM mutation.
Only a small number of families have scripted operation-level coverage.

Actions:

- For each flash family that changes, extract a family-specific session/driver
  over the smallest applicable transport interface.
- Move response validation and block planning into pure byte-native helpers, in
  line with ADR 0004, while keeping Qt conversion at file/serial boundaries.
- Add scripted tests for handshake failure, read success, write cancellation,
  erase/write rejection, stop requests, timeouts, and checksum mismatch before
  changing wire behavior.
- Keep prompts behind `FlashOperationWorker::PromptFn` or a narrower injected
  interface.
- Do not force all ECU families into one state machine unless verified protocol
  behavior demonstrates a stable shared abstraction.

### P1: Narrow serial and hardware interfaces

`IKlineTransport`, `ICanTransport`, and `ISsmTransport` provide byte-native
boundaries for newer protocol code, and the serial facade now marshals calls to
a dedicated I/O thread. Most flash operation classes still receive the full
`SerialPortActions*`, however, and serial/J2534 implementations still combine
port configuration, adapter discovery, protocol mode setup, blocking I/O, and
diagnostics.

Actions:

- Migrate flash family drivers from `SerialPortActions*` to the existing small
  transports plus separate port-configuration and adapter-diagnostics
  interfaces.
- Move the CDBG logging start path's real port/mode setup out of the protocol
  class so the handshake can be scripted headlessly.
- Continue separating J2534 discovery, PE-bitness/bridge lifecycle, PassThru
  types, configuration, and message transport from higher-level serial
  behavior.
- Keep lifecycle coverage for teardown with in-flight calls, helper-process
  failure, timeouts, and adapter removal on each supported platform.

### P1: Drain the `serial_qt_compat` allowlist

The build-graph ratchet for the section above.
`//src/platform/desktop/common/serial:serial_qt_compat` carries
`serial_port_actions.h` to callers that should not have it, and its `visibility`
list is frozen by `scripts/check-serial-compat-allowlist.py`: the list may
shrink, never grow. It currently holds 14 entries — 8 under `src/ui/desktop`,
2 in backend (`//src/backend/flash`, `//src/backend/logging/protocols`), plus
`src/platform/desktop/common/transport`, the serial package itself, and
`//tests`. One entry, `//src/platform/desktop/common/remote_utility`, is not
debt: it is a same-layer sibling using `websocketiodevice.h`/`qtrohelper.hpp`
rather than the serial facade, and is not expected to shrink.

The allowlist makes this debt measurable, which the prose above cannot: each
removed entry is a layer that no longer reaches the full facade. Every entry is
a dependency step 5 (backend) or step 6 (ui) exists to remove.

Actions:

- Split `FlashUtils::configureIso15765Can(SerialPortActions*)` out of
  `src/backend/flash/flash_utils.h` — it is the sole reason the
  `//src/backend/flash` entry survives, and its callers are the relocated
  `src/platform/desktop/common/flash/legacy` CAN operations.
- Remove `FROZEN` entries in `scripts/check-serial-compat-allowlist.py` as the
  matching `visibility` entries are deleted; the check prints the entries to
  drop when the list shrinks.
- Treat a needed new entry as a design failure, not a paperwork step: backend or
  UI code reaching for `serial_port_actions.h` is the dependency being removed.
- Delete `serial_qt_compat` once only the `remote_utility` edge and `//tests`
  remain, and fold its sources into the owning packages.

### P1: Finish the checksum UI boundary

All nine checksum algorithms are portable and return structured
`ChecksumResult` values, but `LegacyChecksumAdapter` still owns direct
`QMessageBox` behavior in the backend layer.

Actions:

- Move checksum result presentation to the desktop UI/application boundary.
- Keep the portable algorithms and dispatch layer free of Qt dependencies.

### P2: Turn static analysis into a ratchet

The Bazel-driven clang-tidy report and autofix commands are useful and covered
by runner tests, but the PR workflow marks the report step
`continue-on-error: true`. New diagnostics can therefore accumulate without a
failing signal.

Actions:

- Record a machine-readable baseline or allowlist by check and source path.
- Fail CI on new diagnostics in changed maintained code, then reduce the
  baseline by ownership area.
- Keep generated Qt code, vendored code, Bazel outputs, and external headers out
  of the baseline.
- Promote the report to a required CI result after the ratchet is deterministic
  across Linux, macOS, and Windows compilation commands.

### P2: Replace BUILD-file globs with generated source lists

56 `glob()` call sites span 28 of the 58 `BUILD.bazel` files. A glob decides
target membership by filename pattern at load time, so adding or renaming a
source silently changes what a target contains — the opposite of the explicit
ownership the package graph was built for. Three failure modes are live:

- **Test sweep.** Twelve library packages use `srcs = glob(["*.cpp"])` with no
  `*_test.cpp` exclusion — the nine under `src/ui/desktop`, both `j2534`
  packages, and `remote_utility`. None contains a test source today, so nothing
  is broken; the first co-located test added to any of them links into the
  production library instead of a test target. Packages that already have tests
  guard against this, but inconsistently — `*_test.cpp`, `test_*.h`, and named
  constants such as `_FLASH_TRANSPORT_SRCS` all appear.
- **MOC partition.** Qt targets pair an explicit `MOC_HDRS` list with
  `normal_hdrs = glob(["*.h"], exclude = MOC_HDRS)`. A new `Q_OBJECT` header
  lands in the globbed half by default, is never moc'd, and fails at link or
  runtime rather than at analysis.
- **Dead code.** `src/ui/desktop` must name `hexcommander.cpp`/`.h` in an
  `exclude` to keep never-built code out of the build; the file's own header
  declares `Q_OBJECT` but is absent from `MOC_HDRS`, so a bare glob would link
  it for the first time and fail.

Adopting Gazelle with a C/C++ extension (`gazelle_cc`) would generate source
lists and `deps` from `#include` analysis, making membership explicit and
reviewable in the diff. Gazelle is not currently a `MODULE.bazel` dependency.

Actions:

- Spike Gazelle on one leaf package before committing to it. The graph is built
  on project macros — `qt_cc_library` (with its `normal_hdrs` split),
  `fastecu_gtest`, `fastecu_portable_gtest`, `qt_ui_basename_libraries`,
  `qt_replica_library` — so adoption needs `# gazelle:map_kind` mappings and,
  for the moc partition, either a custom resolver or a convention that keeps
  `Q_OBJECT` headers derivable. Treat "no workable mapping for the Qt macros"
  as an acceptable outcome that ends this item.
- Until then, add `exclude = ["*_test.cpp"]` to the twelve unguarded `*.cpp`
  globs and settle on one exclusion spelling.
- Keep hand-maintained policy out of anything a generator rewrites: visibility
  allowlists, `target_compatible_with`, the portable/Qt-free `deps` split, and
  the comments explaining them. These are reviewed decisions, not derivable
  facts; mark them `# keep` if Gazelle lands.
- Prefer explicit source lists in new packages regardless of the Gazelle
  outcome.

### P2: Naming and source/data organization

Some names and data placement still reflect earlier architecture:
`log_operations_ssm.cpp` contains MUT/DMA bench utilities, and 31 source/header
files define duplicate `STATUS_SUCCESS`/`STATUS_ERROR` macros.

Actions:

- Move misplaced code into files and namespaces matching current ownership.
- Replace repeated status macros with typed enums or shared result types.
- Remove stale commented-out code while touching nearby behavior.

## Coverage growth sequence toward 80%

1. Definition and configuration logic:
   - Config, logger, RomRaider, and EcuFlash parsers using fixture files.
   - Typed model construction and validation failures.

2. Checksum and calibration logic:
   - Golden vectors and invalid inputs for all checksum families.
   - Calibration-map edit, interpolation, undo/redo, and bounds behavior without
     widgets.

3. I/O and orchestration:
   - Scripted flash-family sessions over K-Line/CAN/SSM transports.
   - Serial/J2534 lifecycle and failure behavior on platform-specific targets.

4. UI boundaries:
   - Thin Qt widget tests for signal wiring, typed command dispatch, and display
     of service results.
   - Do not use UI tests to reach 80%; most coverage should come from extracted
     logic and orchestration.

## Definition of done for new work

- New protocol, math, parser, or model behavior has focused tests or a documented
  bench-only justification.
- New non-UI logic has an owning Bazel library and can be tested without
  constructing `MainWindow`.
- No new pure logic depends directly on `QMessageBox`, `QFileDialog`, or the full
  `SerialPortActions*` facade unless the compatibility reason is documented.
- Coverage and static-analysis commands do not hide unexpected build or test
  failures.
- Generated files and build outputs remain ignored and out of review.
- New debt is added here or to a narrower existing debt document.
