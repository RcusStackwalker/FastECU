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
- Every C++ test is a GoogleTest executable; no QtTest suite remains.
- Tests are strongest around protocol codecs, logging, serial threading, J2534
  bridge behavior, definition parsing, and the config, calibration and
  map-edit use cases. Checksum families, flash workflow orchestration, and UI
  workflows remain lightly or unevenly covered.
- CI builds and tests on Windows, macOS, and Linux, verifies macOS/Windows
  packages, produces coverage for SonarCloud, and runs a blocking clang-tidy
  report over the PR's changed files.
- The [protocol-sharing boundary](design-notes.md#where-port-then-factor-shared-code-and-where-it-did-not)
  lives in the design notes; logging-specific gaps are under
  "P2: Logging engine follow-ups" below.

### Gazelle snapshot

[ADR 0017](adr/0017-generate-bazel-targets-with-gazelle.md) is implemented:
Gazelle generates the source lists and dependencies of the C++ library,
binary and test targets in its scope, every C++ test is covered, and
`scripts/gazelle_check.py` rejects a stale BUILD file or a new whole-rule
keep. Generation itself is established, not debt to pay down further. Architectural
boundaries, target names, visibility, platform selections and the exceptions
documented in the ADR are intentionally hand-owned; change them as design
decisions, not as generation cleanup.

## Priorities

### P1: Separate UI from application logic

The calibration and application logic already exists as portable backend
code: configuration sessions, the calibration service, session and map-edit
use cases, checksums, diagnostics and logging. What remains in the desktop UI
is presentation: `MainWindow` still coordinates write preflight, checksum
interaction, connection orchestration, logging selection, log views, status
updates and dialogs, and the diagnostic-tools windows are set up in
`src/ui/desktop/widgets/menu_actions.cpp`.

Risks:

- Testing presentation flows needs a live `QMainWindow` or `QApplication`.
- Widget selection (table and cell selection, menu actions found by their
  text), dialogs and confirmations, and shared UI state mutation are
  interleaved in central widget code with a large include graph.
- Adding a flash module or application workflow still tends to touch that
  central UI code.

Actions:

- Keep new file, protocol, and hardware logic out of `MainWindow`; route it
  through existing backend use cases. Dialogs and
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
  currently 0 ms (`src/ui/desktop/widgets/dataterminal.cpp`). Pinned, not fixed, in
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
- When the `wrx02` defect above is resolved, confirm it also clears (or at
  least reduces) the SonarCloud `cpp:S3776` cognitive-complexity findings on
  `map_edit.cpp` — see "P2: Pay down the SonarCloud code-smell backlog" below;
  don't track it twice.

### P1: Consolidate flash workflow orchestration

Isolation is complete: every retained flash family is a portable
`FlashPlan`/`IFlashExecutor` pair, registers with `FlashWorkflowFactory`, and
runs through the common `FlashDialog`. Two problems remain: uneven coverage and
duplicated orchestration in the desktop workflows.

Coverage is uneven across families: some carry only the plan/executor unit
tests, with no scripted operation-level (`FlashWorkflow` + `FlashDialog`)
coverage.

Actions:

- Move the remaining hand-rolled single-attempt workflows in
  `src/platform/desktop/common/flash/flash_workflow.cpp` onto the shared
  `SingleAttemptFlashWorkflow`, which already runs the kernel-free CAN,
  kernel-backed and Colt families. Each still repeats the Begin → one
  attempt → result sequence: the Subaru Mitsu/Hitachi M32R K-Line, TCU
  Hitachi M32R K-Line and CAN, Unisia Jecs, Hitachi SH72543R CAN, Hitachi
  SH7058 (an extra read confirmation and a K-Line/CAN transport chosen at
  run time), and Denso MC68HC16Y5 and SH7055 workflows (kernel resolution;
  SH7055 adds an ignition-cycle confirmation). Extend the preparation and
  binding policies only where a family's extra step is genuinely shared.
- Treat the multi-stage workflows as separate work, not part of that
  consolidation: the EEPROM workflow's ignition-cycle and inspect-read
  re-attempts, the Unisia Jecs M32R boot-mode workflow's staged attempts, and
  the programming-voltage apply/remove notices.
- Investigate converging the two remaining per-family `nonfatal_query`
  implementations, in the Denso BEEF CAN executors
  (`subaru_denso_sh7058_can_executor.cpp` and
  `subaru_denso_sh7058_can_diesel_executor.cpp`), onto the shared
  `non_fatal_query` in `uds_client_exchange_common.h`; the other ISO-15765
  executors already wrap it. Both return the reply rather than only logging it
  and differ from the helper, so this is behavior-changing work on
  characterization-tested wire sequences, not a substitution.
- For each flash family that changes, extend its `FlashPlan`/`IFlashExecutor`
  pair rather than adding new orchestration surface.
- Move response validation and block planning into pure byte-native helpers, in
  line with ADR 0004, while keeping Qt conversion at file/serial boundaries.
- Add scripted tests for handshake failure, read success, write cancellation,
  erase/write rejection, stop requests, timeouts, and checksum mismatch before
  changing wire behavior.
- Do not force all ECU families into one state machine unless verified protocol
  behavior demonstrates a stable shared abstraction.

### P1: Narrow serial and hardware interfaces

Portable flash executors already receive typed transports
(`IKlineFlashTransport`, `ICanFlashTransport` and `IMixedCanFlashTransport`), and
`IKlineTransport`, `ICanTransport` and `ISsmTransport` give the logging,
diagnostics and protocol code byte-native boundaries; no code under
`src/backend` includes the serial facade. The remaining work is on the desktop
side: the `SerialPortActions` facade, which marshals calls to a dedicated I/O
thread, and the direct serial/J2534 implementation behind it, which still
combines port configuration, adapter discovery, protocol mode setup, blocking
I/O, and diagnostics in `SerialPortActionsDirect`.

The facade is platform-only: `serial_port_actions` is visible to
`src/platform/desktop` and the test layer alone, and adapters take it through
`implementation_deps`. UI tests still see its headers through `FakeBackend`,
which derives from `SerialPortActionsDirect`.

Actions:

- Continue separating J2534 discovery, PE-bitness/bridge lifecycle, PassThru
  types, configuration, and message transport from higher-level serial
  behavior in the direct implementation.
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

Three files still suppress `bugprone-signed-bitwise` behind
`NOLINTBEGIN`/`NOLINTEND` blocks because legacy code mixes signed literals,
loop counters, `QByteArray::at()` results, or vendor J2534 flag macros into
bitwise operations. Each block's comment points back to this section.
clang-tidy's changed-file gate (`bazel run //:clang_tidy_report_changed`)
lints whole translation units, not touched lines, so any edit to one of these
files puts all of its existing findings back in scope; each suppression was
added when a step needed to touch the file for an unrelated reason. Re-run
clang-tidy on a file for its current findings rather than relying on a count
recorded here.

- `src/ui/desktop/widgets/get_key_operations_subaru.cpp` (the Subaru
  key-recovery dialog): one block spanning the linear-approximation and
  cipher helpers. None of the findings is a live defect under C++23 (signed
  left shifts such as `roundFunction`'s promoted `uint16_t << 16` wrap modulo
  2^32 since C++20), but the code only works because every operand happens to
  be non-negative.
- `src/platform/desktop/common/serial/direct/common/serial_port_actions_direct.cpp`:
  one block per function around `read_serial_data`, `append_iso14230_header`,
  `write_j2534_data`, `read_j2534_data`, `dump_msg` and `set_j2534_iso9141`.
  The J2534 flag macros involved (`TX_DONE`, `START_OF_MESSAGE`,
  `ISO15765_FRAME_PAD`, `ISO9141_NO_CHECKSUM`, `CAN_ID_BOTH`) are small
  positive constants, and the `QByteArray` byte/mask sites mask or truncate to
  low bits that sign extension does not affect; none is a live defect.
- `src/ui/desktop/widgets/dataterminal.cpp`: blocks around `sendToInterface`
  and `add_ssm_header`, where a `uint8_t`/`toUInt()` id is ANDed or shifted
  with a small non-negative mask; none is a live defect.

Actions:

- For `get_key_operations_subaru.cpp`: extract the pure cipher helpers
  (`get_bit`, `sBox`, `fFunction`, `roundFunction`, `flipLeftRight`,
  `manyRoundAndFlip`) out of the dialog into a free-function unit with a
  co-located test, pin their current outputs with characterization
  vectors, then convert the operands to unsigned types and remove the
  suppression block.
- For the other two files, convert each flagged site's operand types
  (the protocol id/length fields and the relevant J2534 macros' consumers)
  to unsigned, confirm the existing protocol/serial tests still pass, and
  remove the corresponding suppression blocks. These functions have
  hardware/QObject side effects rather than pure logic, so the
  extract-and-characterize step above does not apply the same way; a
  direct signed-to-unsigned conversion plus existing test coverage is
  enough.

### P2: Pay down the SonarCloud code-smell backlog

No current totals are recorded here: earlier per-rule counts predate large
deletions and migrations. **Obtain a fresh scan before scheduling any of it**
(for example `sonar list issues --project RcusStackwalker_FastECU --statuses
OPEN,CONFIRMED --format json`, 500 per page) and work from its results. The
rules named below were where findings concentrated in earlier scans; confirm
each still applies before acting on it.

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
  complexity) on `map_edit.cpp` is revisited with the `wrx02` defect above.
  Scattered `S3776`/`S134` findings stay a plain backlog; re-run the query when
  picking up a file.
- Duplication clusters not yet extracted: the dialog→validate→write tail of
  the two wizards in `src/ui/desktop/definition/dialog/definition_authoring_dialog.cpp`,
  and the transport adapters in `src/platform/desktop/common/transport/`,
  whose read/write guards differed only by a label string and need
  re-measuring.
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
- `LoggingEngine::stop()` publishes `SessionEndReason::StoppedByUser` itself
  after disconnecting the worker's signals, so the engine's mapping of a
  worker `Cancelled` result to `StoppedByUser` is not reached from `stop()`.

Minor code-level findings (confirmed against the code when this roadmap was
last refreshed; re-check before scheduling):

- `src/backend/protocol/issm_transport.h` defines `ISsmTransport` in the global
  namespace, unlike `mutdma::IKlineTransport` and `cdbg::ICanTransport`.
- `FastEcuSsmTransport::write()` discards the bytes returned by
  `write_serial_data_echo_check()` and reports the input size unconditionally,
  so an echo failure is not exposed.
- `MainWindow::restoreLoggingUiState()` (`src/ui/desktop/widgets/mainwindow.cpp`)
  finds the menu action whose text is `Logging`; the same text-based lookup is
  repeated in `set_identification_in_progress()`, `set_realtime_state()` and
  `toggle_realtime()`, and
  `toggle_log_to_file()` looks up `Log to file` the same way
  (`src/ui/desktop/widgets/menu_actions.cpp`).

### P2: Naming and source/data organization

Some names and data placement still reflect earlier architecture.

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
- Coverage exclusions stay explicit and reviewed: tests, generated Qt files,
  vendored `src/ui/desktop/hexedit/`, Bazel/external outputs, system libraries,
  and platform SDKs.
- Generated files and build outputs remain ignored and out of review.
- New debt is added here or to a narrower existing debt document.
