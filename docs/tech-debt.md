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

- FastECU is a Qt 6/C++23 desktop application. Bazel is the sole target graph for
  the application, tests, release packaging, coverage, compile commands, and
  clang-tidy (ADR 0001).
- Tests are strongest around protocol codecs, logging, serial threading, J2534
  bridge behavior, definition parsing, and the config, calibration and
  map-edit use cases. Checksum families, most flash orchestration, and UI
  workflows remain lightly covered.
- CI builds and tests on Windows, macOS, and Linux, verifies macOS/Windows
  packages, produces coverage for SonarCloud, and runs a blocking clang-tidy
  report over the PR's changed files.
- The [protocol-sharing boundary](design-notes.md#where-port-then-factor-shared-code-and-where-it-did-not)
  lives in the design notes; logging-specific gaps are under
  "P2: Logging engine follow-ups" below.

## Priorities

### P0: Make coverage results trustworthy

Remaining gaps:

- The package-owned serial tests inherit an intermittent Windows-only crash
  from the former aggregate `serial_backend_tests` target. The split targets
  retain unbuffered diagnostics so the failing binary and slot can be isolated;
  until then, the crash should not be attributed to one suite or used as a
  reason to ignore unrelated coverage-test failures.
- An empty Windows `test.log` is **not** evidence of a crash. QtTest's own
  transcript does not reach the stdout Bazel captures under the default
  logger, so a QtTest suite's Windows log is empty whether it passed or
  failed, and making stdio unbuffered does not change that. Verified on CI:
  a build whose `main` wrote progress markers to stderr showed every marker,
  including one printed after `qExec` returned, while the QtTest banner, the
  `PASS` lines and the totals were all absent. To read a Windows failure,
  pass `-o <file>,txt` to `qExec` and echo that file to stderr; that is how
  the Windows-only failure fixed in #343 was finally diagnosed, after seven
  runs whose empty logs had been read as silent crashes.

Actions:

- Resolve or explicitly quarantine the intermittent serial backend test with a
  separate visible CI result and an owner; do not silently discard its exit
  status.
- Keep exclusions explicit and reviewed: tests, generated Qt files, vendored
  `src/ui/desktop/hexedit/`, Bazel/external outputs, system libraries, and platform SDKs.

### P1: Separate UI from application logic

`MainWindow` remains the central coordinator for presentation: write preflight,
checksum interaction, connection orchestration, logging selection, log views,
status updates, and dialogs. The diagnostic-tools window setup lives in
`menu_actions.cpp`.

Risks:

- Most workflows require a live `QMainWindow` or `QApplication` to test.
- Adding a flash module or application workflow tends to touch central UI code
  and its large include graph.
- Calibration rules and command behavior remain mixed with widget lookup,
  message boxes, table selection, and shared state mutation.

Actions:

- Keep new file, protocol, and hardware logic out of `MainWindow`. Dialogs and
  confirmations for write preflight and checksum correction stay in the UI;
  post-read and checksum/save/write bench re-verification remain required
  before release, and the logger identity corrections still await
  [bench qualification](design-notes.md#logger-ownership-and-stable-identities).
- **Confirm or fix the OpenPort five-baud ASCII comparison.**
  `five_baud_header` (`src/backend/diagnostics/obd_frames.cpp`) preserves the
  J2534 branch's comparison of response bytes `[5]`/`[7]` (iso9141) and
  `[8]`/`[9]` (iso14230) against the ASCII characters `'8'`/`'8'` and
  `'8'`/`'f'` -- different offsets from the direct-serial branch's numeric
  `0x08`/`0x08` (bytes `[1]`/`[2]`) and `0x8F` (byte `[2]`) comparison, and a
  match only by coincidence of digit value. Confirm against a bench capture
  whether the OpenPort firmware genuinely echoes ASCII here before changing
  it; see the [design notes](design-notes.md#diagnostic-tools).
- **Fix DataTerminal's `delay(...)` script parser.** `split(")").at(1).split("(").at(0)`
  parses `delay(100)` to an empty string, so every scripted delay is
  currently 0 ms (`src/ui/desktop/dataterminal.cpp`). Pinned, not fixed, in
  step 6g because a real parse would change the timing of every existing
  delay script; see the [design notes](design-notes.md#diagnostic-tools).
- **DTC session test gaps.** `dtc_session_test.cpp` does not yet cover: the
  clear-loop's short-frame/NRC/wrong-ID paths; a CAN-init short response or a
  non-`0x41` CAN-init response; a fast-init read cancelled mid-flight; and
  several `IEventSink::log` strings, which are unasserted. Not done as of
  step 6g; add scripted `FakeDiagnosticLink` cases for each before relying on
  this coverage for a protocol change.
- **`mutdma::read_memory`/`write_memory` have two unresolved behaviors from
  the `MainWindow` originals they were moved from (step 6g,
  `//src/backend/protocol:mut_memory`), unchanged and uncalled.**
  `read_memory` can return a gapped buffer: a chunk whose poll yields no
  frame within its timeout contributes nothing to the output and the read
  continues with the next chunk, silently omitting that range rather than
  retrying or flagging it. `write_memory`'s `0x4000`-`0xBFFF` guard checks
  only the start address, not the end of the write, so a write starting
  inside the window can still extend past `0xBFFF`. Revisit both before
  wiring a caller; never relax the guard.
- **Fix or defer the `wrx02` write-path predicate (step 6b defect (a); see
  the [design notes](design-notes.md#calibration-defect-letters)).** `element_byte_address` (`src/backend/calibration/map_edit.cpp`)
  still carries two different predicates for the `wrx02` flash-method
  address fixup depending on its `for_write` parameter — one for reads, one
  for writes — subtracting `0x8000` under different conditions; a cell near
  the boundary can display one byte and write a different one. The
  write-side predicate matches the documented `apply_flash_method_padding`
  rule (inserting `0x8000` bytes at `0x20000` for images under `190 * 1024`;
  see `calibration_service.h`), which suggests the read side is the wrong
  copy, but step 6b required confirming that against a real `wrx02` definition before landing a fix rather than
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
- Whichever `map_edit.cpp` defect above is resolved first, confirm it also
  clears (or at least reduces) the SonarCloud `cpp:S3776` cognitive-complexity
  findings on the same functions — see "P2: Pay down the SonarCloud
  code-smell backlog" below; don't track it twice.

### P1: Isolate flash-operation orchestration

Every flash family registers with `FlashWorkflowFactory` and runs through the
common `FlashDialog`. Coverage is uneven across families: some carry only the
plan/executor unit tests, with no scripted operation-level (`FlashWorkflow` +
`FlashDialog`) coverage.

Actions:

- For each flash family that changes, extend its `FlashPlan`/`IFlashExecutor`
  pair rather than adding new orchestration surface.
- Move response validation and block planning into pure byte-native helpers, in
  line with ADR 0004, while keeping Qt conversion at file/serial boundaries.
- Add scripted tests for handshake failure, read success, write cancellation,
  erase/write rejection, stop requests, timeouts, and checksum mismatch before
  changing wire behavior.
- Do not force all ECU families into one state machine unless verified protocol
  behavior demonstrates a stable shared abstraction.
- Unify the near-duplicate workflow classes in
  `src/platform/desktop/common/flash/flash_workflow.cpp`:
  `KernelBackedCanFlashWorkflow` duplicates the sibling
  `SimpleCanFlashWorkflow` plus lazy kernel resolution, a confirmation loop
  and a transport parameter, and `ColtWorkflow` hand-rolls the same
  confirmation loop. They are three of the file's sixteen sibling
  `FlashWorkflow` classes. This was deferred
  because it changes routing for every merged CAN family and needs its own
  risk budget; `single_window_plan` was considered and rejected as the vehicle.
- Investigate converging the two remaining per-family `nonfatal_query`
  implementations, in the Denso BEEF CAN executors
  (`subaru_denso_sh7058_can_executor.cpp` and
  `subaru_denso_sh7058_can_diesel_executor.cpp`), onto the shared
  `non_fatal_query` in `uds_client_exchange_common.h`; the other ISO-15765
  executors already wrap it. Both return the reply rather than only logging it
  and differ from the helper, so this is behavior-changing work on
  characterization-tested wire sequences, not a substitution.

### P1: Narrow serial and hardware interfaces

`IKlineTransport`, `ICanTransport`, and `ISsmTransport` provide byte-native
boundaries for newer protocol code, and the serial facade now marshals calls to
a dedicated I/O thread. Most flash operation classes still receive the full
`SerialPortActions*`, however, and serial/J2534 implementations still combine
port configuration, adapter discovery, protocol mode setup, blocking I/O, and
diagnostics.

The facade is platform-only: `serial_port_actions` is visible to
`src/platform/desktop` and `//tests` alone, and adapters take it through
`implementation_deps`. UI tests still see its headers through `FakeBackend`,
which derives from `SerialPortActionsDirect`.

Actions:

- Continue separating J2534 discovery, PE-bitness/bridge lifecycle, PassThru
  types, configuration, and message transport from higher-level serial
  behavior.
- Keep lifecycle coverage for teardown with in-flight calls, helper-process
  failure, timeouts, and adapter removal on each supported platform.

### P2: Identify Subaru CAN ECUs with SSM `AA`

Step 6h kept CAN identification byte-faithful: iso15765 sends UDS
`22 F1 82` and gets an ID but no capability bits, so CAN logging never
filters log values by what the ECU supports, and raw CAN identifies nothing
(its legacy branch tested a protocol value no configuration sets). RomRaider
identifies over CAN with SSM `AA` to 0x7E0 and gets `EA` plus the same
capability bytes K-Line returns.

Actions:

- Capture an `AA`/`EA` exchange on a bench CAN ECU before changing anything.
- Add an `SsmVariant` for it in `identify_ssm_ecu`, validated like SSM2, and
  route both CAN transports to it.
- Qualify it on the [connection bench checklist](connection-bench-checklist.md).

### P2: Convert suppressed signed-bitwise arithmetic to unsigned operands

Four files suppress `bugprone-signed-bitwise` behind
`NOLINTBEGIN`/`NOLINTEND` blocks because legacy code mixes signed literals,
loop counters, `QByteArray::at()` results, or vendor J2534 flag macros into
bitwise operations. clang-tidy's changed-file gate (`bazel run
//:clang_tidy_report_changed`) lints whole translation units, not touched
lines, so any edit to one of these files — however small — puts its full
existing finding count back in scope; each suppression below was added
when a step needed to touch the file for an unrelated reason.

- `src/ui/desktop/get_key_operations_subaru.cpp` (the Subaru key-recovery
  dialog): 39 findings, suppressed when step 6c-1 touched the file. None
  is a live defect under C++23 (signed left shifts such as
  `roundFunction`'s promoted `uint16_t << 16` wrap modulo 2^32 since
  C++20), but the code only works because every operand happens to be
  non-negative.
- `src/ui/desktop/widgets/dtc_operations.cpp`: 11 findings across five functions,
  suppressed when step 6e-1 touched the file — `iso15765_init` (3),
  `request_data` (2), `request_vehicle_info` (2), `request_dtc_list` (2),
  `clear_dtc` (2). All combine a `uint16_t`/`uint8_t` protocol field
  (`source_id`, `cmd`, a `QByteArray::at()` byte) with a small non-negative
  mask or constant; none is a live defect.
- `src/platform/desktop/common/serial/direct/common/serial_port_actions_direct.cpp`: 8
  findings across six functions, suppressed when step 6e-1 touched the
  file — `read_serial_data` (2), `append_iso14230_header` (1),
  `write_j2534_data` (1), `read_j2534_data` (2), `dump_msg` (1),
  `set_j2534_iso9141` (1). The J2534 flag macros involved (`TX_DONE`,
  `START_OF_MESSAGE`, `ISO15765_FRAME_PAD`, `ISO9141_NO_CHECKSUM`,
  `CAN_ID_BOTH`) are all small positive constants, and the `QByteArray`
  byte/mask sites (`received.at(0) & 0x3f`, `output[0] = output[0] |
  msglength`) mask or truncate to the low bits that are unaffected by sign
  extension either way; none is a live defect.
- `src/ui/desktop/dataterminal.cpp`: 4 findings across two functions,
  suppressed when step 6e-1 touched the file — `sendToInterface` (2),
  `add_ssm_header` (2). Same pattern: a `uint8_t`/`toUInt()` id ANDed or
  shifted with a small non-negative mask; none is a live defect.

Actions:

- For `get_key_operations_subaru.cpp`: extract the pure cipher helpers
  (`get_bit`, `sBox`, `fFunction`, `roundFunction`, `flipLeftRight`,
  `manyRoundAndFlip`) out of the dialog into a free-function unit with a
  co-located test, pin their current outputs with characterization
  vectors, then convert the operands to unsigned types and remove the
  suppression block.
- For the other three files, convert each flagged site's operand types
  (the protocol id/length fields and the relevant J2534 macros' consumers)
  to unsigned, confirm the existing protocol/serial tests still pass, and
  remove the corresponding suppression block. These functions have
  hardware/QObject side effects rather than pure logic, so the
  extract-and-characterize step above does not apply the same way; a
  direct signed-to-unsigned conversion plus existing test coverage is
  enough.

### P2: Pay down the SonarCloud code-smell backlog

The last recorded scan (2026-09-06, via `sonar list issues --project
RcusStackwalker_FastECU --statuses OPEN,CONFIRMED --format json`, 500 per
page) found 3,870 open issues, all `CODE_SMELL` (no bugs or vulnerabilities),
about 517 estimated remediation hours. Its per-rule counts predate the deletion
of the legacy per-vendor flash-operation files, the parallel-list structs and
the `FileActions` family, which held most of the top findings, so **re-run the
scan before scheduling any of it** and discard counts that no longer apply.

No ratchet job is needed to stop the backlog growing: the project's "Sonar
way" quality gate is Clean-as-You-Code, and its `new_maintainability_rating`
condition already fails a PR that introduces enough new code-smell debt.
Order the paydown by risk, not by count:

- **Correctness-risk triage first.** `cpp:S1117` (shadowed declarations),
  `cpp:S5276` (implicit narrowing) and `cpp:S5025` (raw `new`/`delete`)
  concentrated in the hardware-facing layer, and some `S1117` messages named
  variables that looked copy-paste-shadowed rather than intentionally reused.
  Treat each surviving instance as a triage question, not a mechanical rename:
  classify it cosmetic or suspicious (an outer variable's intended assignment
  never happens); pin suspicious ones with a characterization test before
  changing them; convert `new`/`delete` pairs scoped to one function to RAII
  and give cross-function or cross-thread ownership a closer review.
- **Mechanical Critical cleanup.** `cpp:S5028` (macro should be
  `const`/`constexpr`/an enum) was concentrated in `J2534_tactrix_unix.h` and
  `kernelcomms.h`; a pure type change with no value change.
- **Bulk modernization.** `cpp:S6022` (`std::byte`), `cpp:S5945` (C array to
  `std::array`/`std::vector`) and `cpp:S125` (commented-out code): fix per
  file in one pass, folded into whichever file is already open, rather than one
  rule across all files.
- **Structural rules ride on existing items.** `cpp:S3776` (cognitive
  complexity) on `map_edit.cpp` clears with the `map_edit.cpp` defects above.
  Scattered `S3776`/`S134` findings stay a plain backlog; re-run the query when
  picking up a file.
- Duplication clusters not yet extracted: the dialog→validate→write tail of
  the two wizards in `src/ui/desktop/definition/definition_authoring_dialog.cpp`,
  and the five adapters in `src/platform/desktop/common/transport/`, whose
  read/write guards differed only by a label string and need re-measuring.
- `WriteSelection.ReproducesTheFourSpaceQDomIndent`
  (`src/backend/logging/logger_conf_test.cpp`) pins pugixml output against
  bytes captured from the deleted `QDomDocument::save(output, 4)` writer. It
  was transitional, proving the logger-conf migration once. Replace it with a
  golden regenerated from pugixml's own output plus the existing
  `write_selection` → `read_selection` round trip; keep the four-space
  `format_indent` setting itself.

### P2: Logging engine follow-ups

Findings specific to the `LoggingProtocol`/`LoggingWorker`/`LoggingEngine`
architecture. Hardware qualification is gated by the
[logging engine bench checklist](logging-engine-bench-checklist.md); the most
consequential open checks are:

1. **Plain-serial logging through `SerialIoThread`.** The backend owns
   `QSerialPort` on its dedicated I/O thread and headless PTY coverage exists,
   but continuous logging, adapter removal, and clean teardown still need
   confirmation with a real non-J2534 adapter and ECU.
2. **SSM's per-cycle `0xA8 0x01` request.** `SsmLoggingProtocol::poll()` sends
   the request on every cycle. Confirm that supported ECUs treat it as strict
   request/response rather than entering a continuous stream that repeated
   requests could desynchronize.
3. **CDBG logging.** Raw-CAN setup, handshake, security access, and stream
   behavior remain gated by the
   [CDBG CAN bench checklist](cdbg-can-logging-bench-checklist.md).

The worker-thread prompts and progress reporting used by Mitsubishi M32R CAN
flashing are tracked in the
[Colt CZT CAN bench checklist](colt_czt_47110032_can_bench_checklist.md).

Deferred behavior:

- No live GUI indicator for `LoggingStatus::CarNotResponding`: the worker and
  engine emit it, but `MainWindow` shows only a warning in the log window.
- No live reconfiguration: changing channels, poll interval, or protocol
  requires a stop/start; live changes would need an explicit path through
  `LogSessionConfig` and `LoggingWorker`.
- `SessionEndReason::StoppedByUser` is not observed by production callers,
  because `LoggingEngine::stop()` disconnects worker signals before requesting
  the stop. The worker branch is tested and remains a standalone contract.

Minor code-level findings (verify before scheduling; these were recorded
before several migrations):

- `src/backend/protocol/issm_transport.h` defines `ISsmTransport` in the global
  namespace, unlike `mutdma::IKlineTransport` and `cdbg::ICanTransport`.
- `FastEcuSsmTransport::write()` discards the bytes returned by
  `write_serial_data_echo_check()` and reports the input size unconditionally,
  so an echo failure is not exposed.
- `MainWindow::handleLoggingSessionEnded()` finds the menu action whose text is
  `Logging`; similar text-based lookup is duplicated in `toggle_realtime()` and
  `toggle_log_to_file()`.
- `test_ssm_logging_protocol` includes timeout-bounded cases that wait on real
  elapsed time; watch its runtime as timeout scenarios are added.

### P2: Naming and source/data organization

Some names and data placement still reflect earlier architecture:
`log_operations_ssm.cpp` contains MUT/DMA bench utilities.

Actions:

- Move misplaced code into files and namespaces matching current ownership.
- Replace the facade's integer `STATUS_*` return codes with typed enums or
  shared result types, under names that cannot collide with `ntstatus.h`.
- Remove stale commented-out code while touching nearby behavior.

## Coverage growth sequence toward 80%

1. Definition and configuration logic:
   - Config, logger, RomRaider, and EcuFlash parsers using fixture files.
   - Typed model construction and validation failures.

2. Checksum and calibration logic:
   - Golden vectors and invalid inputs for all checksum families.
   - Calibration-map undo/redo behavior without widgets: it stays a
     `qDebug()` stub in the UI.

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
