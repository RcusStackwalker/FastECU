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
  bridge behavior, definition parsing, the extracted config/calibration use
  cases, and, as of step 6b, calibration map-edit/interpolation/bounds
  behavior. Checksum families, most flash orchestration, and UI workflows
  remain lightly covered.
- CI builds and tests on Windows, macOS, and Linux, verifies macOS/Windows
  packages, produces coverage for SonarCloud, and runs a blocking clang-tidy
  report over the PR's changed files.
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
2.7k lines and `menu_actions.cpp` is down to 1,315 lines (from ~2.1k) after
step 6b extracted the map-edit arithmetic into
`//src/backend/calibration:map_edit`.

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
- ~~Move calibration/map commands out of `menu_actions.cpp` into headless
  model operations; leave only selection extraction, signal wiring, and user
  feedback in the UI.~~ Done (step 6b): the byte codec, target resolution,
  display helpers, and all four edit operations
  (increment/set-expression/interpolation/paste) now live in
  `//src/backend/calibration:map_edit`, reached from `menu_actions.cpp`
  through resolve → collect input → call → apply patch → repaint. Undo/redo
  is a deliberate non-goal of that slice (see the design's own "Also
  non-goal" note) and is not covered by this action.
- Keep new file, protocol, and hardware logic out of `MainWindow`.
- **Fix or defer the `wrx02` write-path predicate (step 6b spec defect
  (a)).** `element_byte_address` (`src/backend/calibration/map_edit.cpp`)
  still carries two different predicates for the `wrx02` flash-method
  address fixup depending on its `for_write` parameter — one for reads, one
  for writes — subtracting `0x8000` under different conditions; a cell near
  the boundary can display one byte and write a different one. The
  write-side predicate matches the documented `apply_flash_method_padding`
  rule (inserting `0x8000` bytes at `0x20000` for images under `190 * 1024`;
  see `calibration_service.h`), which suggests the read side is the wrong
  copy, but the step 6b design (spec section "(a)") requires confirming that
  against a real `wrx02` definition before landing a fix rather than
  guessing. Measurement taken while closing out step 6b: `grep -rl wrx02`
  across both the `mmc-definitions` and `mmc-patches` corpora finds **zero**
  ROMs anywhere that declare `wrx02` as their flash method, so there is no
  real definition to confirm the fix against, and the fix stays deferred
  with `PinnedDefect_Wrx02FixupDiffersBetweenReadAndWrite`
  (`src/backend/calibration/map_edit_test.cpp`) still pinned, unflipped.
  Landing this needs either a real `wrx02` definition surfacing later to
  confirm which predicate is correct, or an explicit accepted-risk decision
  to pick the write-side predicate on the padding-rule reasoning alone
  without that confirmation.
- **Fix `resolve_edit_target`'s `y_size == 1` column shift, which can still
  produce an out-of-bounds read.** `resolve_edit_target`
  (`src/backend/calibration/map_edit.cpp`) shifts rows back by one on the
  X-axis branch to skip a 3D map's header row but never shifts columns —
  correct for a 3D map, wrong for a "2D" map with `y_size == 1`, which has no
  header column (`calibration_maps.cpp`'s `xSizeOffset = 0`). Selecting that
  layout's sole X-axis breakpoint yields `range.first_col == -1`, which
  becomes a cell index of `static_cast<std::uint32_t>(-1)` — a huge value once
  unsigned. `apply_patch`
  (`src/ui/desktop/calibration/map_edit_adapter.cpp`) was hardened against the
  resulting out-of-bounds *write* by dropping such a cell, but the edit
  operations themselves (`src/backend/calibration/map_edit.cpp`) still perform
  an unchecked out-of-bounds *read* on the `cell_text` span for the same
  index: `apply_set_expression` and `apply_increment` index it directly, and
  `apply_interpolation` does so through its `cell_at(...)` helper.
  (`apply_paste` does not read `cell_text` at all, and every ROM-side read
  goes through `read_raw_element`, which is bounds-checked.) That is
  a live memory-safety hazard, and the `apply_patch` guard narrows the class
  of harm rather than closing it. It is pre-existing behavior in code step 6b
  moved rather than introduced, which is why 6b-4 recorded it instead of
  fixing it: the real fix is in `resolve_edit_target`'s own `y_size == 1`
  branch, not in per-caller bounds checks, and changing which element a
  selection resolves to is a behavior change that needs its own test rather
  than riding along with a defect-fix PR. Landing this means correcting the
  column shift for the `y_size == 1` layout, covering it with a
  `resolve_edit_target` test asserting the corrected range, and then
  confirming the `apply_patch` guard has become unreachable rather than
  load-bearing.

### P1: Split `FileActions`

`FileActions` (`src/backend/definitions/file_actions.{h,cpp}`, 986 lines plus
a 236-line header) no longer inherits `QWidget`, declares no `Q_OBJECT`, and
constructs no dialog or message box — `//:backend_no_widgets` enforces this
for all of `src/backend`. Expression and diagnostic parsing, the
EcuFlash/RomRaider parsers, ROM open/save, and config persistence have been
extracted into portable use cases under
`src/backend/{definition,calibration,config}/`, each reached through a
`Legacy*Adapter`; checksum dispatch has also been extracted, under
`src/backend/checksum/`, but is reached directly from the desktop UI's
`ChecksumCorrectionCommand` rather than through a `Legacy*Adapter`. What
remains inside `FileActions` is logger definition/conf reading, config
persistence, the EcuFlash/RomRaider definition-lookup parsers, ROM open/save,
and the nested `ConfigValuesStructure` / `LogValuesStructure` /
`EcuCalDefStructure` models, still `QString`/`QStringList` typed.

Actions:

- Extract `LoggerDefinitionParser` as a non-widget component with explicit
  inputs and results, following the definition-parser precedent.
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

Portable Colt CAN, Subaru Mitsubishi M32R K-Line, and Denso SH705x EEPROM operations now register with
`FlashWorkflowFactory` and run through the common `FlashDialog`. Remaining
flash/eeprom/jtag/bdm operation pairs use `FlashOperationWorker`, and
shared SSM framing, seed/payload transforms, CRC, byte formatting, byte
stuffing, and ISO-15765 setup have been consolidated. The remaining safe
generalization opportunities are maintained in the
[protocol generalization notes](protocol-generalization-opportunities.md).

The operation classes still combine request construction, response validation,
retries, progress reporting, prompts, full-facade serial I/O, and ROM mutation.
Wave 1 is in progress: the Mitsubishi M32R K-Line sibling is portable while
Hitachi M32R K-Line and any shared cluster factoring remain follow-ups. The
legacy drain contains 25 operation sources. Only a small number of families
have scripted operation-level coverage.

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
shrink, never grow. It currently holds 13 entries — 8 under `src/ui/desktop`,
0 in backend (step 5e relocated the former `//src/backend/flash` entry to
`//src/platform/desktop/common/flash/legacy`, which now carries the
flash-family debt), plus `src/platform/desktop/common/transport`, the serial
package itself, `//src/platform/desktop/common/flash/legacy`, and `//tests`.
One entry, `//src/platform/desktop/common/remote_utility`, is not debt: it is
a same-layer sibling using `websocketiodevice.h`/`qtrohelper.hpp` rather than
the serial facade, and is not expected to shrink.

The allowlist makes this debt measurable, which the prose above cannot: each
removed entry is a layer that no longer reaches the full facade. Every entry is
a dependency step 5 (backend) or step 6 (ui) exists to remove.

Actions:

- Remove `FROZEN` entries in `scripts/check-serial-compat-allowlist.py` as the
  matching `visibility` entries are deleted; the check prints the entries to
  drop when the list shrinks.
- Treat a needed new entry as a design failure, not a paperwork step: backend or
  UI code reaching for `serial_port_actions.h` is the dependency being removed.
- Delete `serial_qt_compat` once only the `remote_utility` edge and `//tests`
  remain, and fold its sources into the owning packages.

### P2: Turn static analysis into a ratchet

`pr.yml` gates every PR on `//:clang_tidy_report_changed` (changed files
only, `WarningsAsErrors: '*'`, blocking, per OS). Full-repo
`clang_tidy_report`/`clang_tidy_fix` remain available as manual targets but
no longer run anywhere in CI, so a pre-existing violation in code a PR
doesn't touch has no CI signal at all.

Actions:

- Record a machine-readable baseline or allowlist by check and source path
  for the full-repo report.
- Add a scheduled or release-triggered job that runs the full-repo report
  against that baseline and fails only on new diagnostics outside it, then
  reduce the baseline by ownership area over time.
- Keep generated Qt code, vendored code, Bazel outputs, and external headers
  out of the baseline.
- Promote that job to a required, blocking result once the ratchet is
  deterministic across Linux, macOS, and Windows compilation commands.

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
   - ~~Calibration-map edit, interpolation, undo/redo, and bounds behavior
     without widgets.~~ Covered by step 6b for edit, interpolation, and bounds
     (`//src/backend/calibration:map_edit`'s `fastecu_portable_gtest` suite —
     byte codec, target resolution, all four edit operations, and
     bounds/saturation guards, no Qt). Undo/redo remains uncovered: it stays
     a `qDebug()` stub in the UI, a deliberate non-goal of step 6b.

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
