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

`MainWindow` remains the central coordinator for startup, settings/config
loading, serial/device setup, logging wiring, calibration lifecycle, ECU
operation dispatch, log views, status updates, and dialogs. Since step 6d
`mainwindow.h` includes no flash dialog, and flash dispatch runs through
`FlashOperationController`. `mainwindow.cpp` is about
2.5k lines (2,473 after step 6d) and `menu_actions.cpp` is down to 1,315 lines (from ~2.1k) after
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
- Flash dispatch runs through `FlashOperationController` (step 6d); what
  remains in `MainWindow::start_ecu_operations` is write preflight,
  checksum correction, and the post-read calibration handoff, which all
  mutate `ecuCalDef` and move with the "Replace parallel-list data models"
  work rather than on their own.
- Keep new file, protocol, and hardware logic out of `MainWindow`.
- Logging-protocol registration is composition-owned as of step 6f. The
  platform transport package builds the three protocols; the UI passes the
  ECU/TCU choice in the logging snapshot. Connection orchestration,
  protocol/policy selection, and log-file handling still live in the UI.
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

### P1: Split `FileActions`

`FileActions` (`src/backend/definitions/file_actions.{h,cpp}`, 986 lines plus
a 236-line header) no longer inherits `QWidget`, declares no `Q_OBJECT`, and
constructs no dialog or message box — Qt Widgets are unreachable from all of
`src/backend`, enforced by the visibility of the `//bazel/qt:widgets` alias
([ADR 0016](adr/0016-enforce-qt-reachability-by-visibility.md)). Expression and diagnostic parsing, the
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
- Confirm this clears SonarCloud's `cpp:S1820` (struct exceeds 20 fields) on
  `ConfigValuesStructure`, `LogValuesStructure`, and `EcuCalDefStructure` — see
  "P2: Pay down the SonarCloud code-smell backlog" below; don't track it twice.

### P1: Isolate flash-operation orchestration

Every flash family — ECU, TCU, EEPROM, JTAG (removed), BDM, and bootmode —
now registers with `FlashWorkflowFactory` and runs through the common
`FlashDialog`. `FlashOperationWorker` and the per-family legacy operation
classes it backed are gone: the step 5 tail's wave 7 deleted the package
(`src/platform/desktop/common/flash/legacy/`) along with the drain ratchet
that tracked it. Shared SSM framing, seed/payload transforms, CRC, byte
formatting, byte stuffing, and ISO-15765 setup have been consolidated. The
remaining safe generalization opportunities are maintained in the
[protocol generalization notes](protocol-generalization-opportunities.md).

Coverage is uneven across families: some carry only the plan/executor unit
tests each wave added, with no scripted operation-level (`FlashWorkflow` +
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
- Investigate converging the per-family `nonfatal_query` implementations in the
  Denso and Hitachi ISO-15765 executors onto the shared `non_fatal_query` in
  `uds_client_exchange_common.h`. Every version differs from the others and
  from the helper, so this is behavior-changing work on characterization-tested
  wire sequences, not a substitution.

### P1: Narrow serial and hardware interfaces

`IKlineTransport`, `ICanTransport`, and `ISsmTransport` provide byte-native
boundaries for newer protocol code, and the serial facade now marshals calls to
a dedicated I/O thread. Most flash operation classes still receive the full
`SerialPortActions*`, however, and serial/J2534 implementations still combine
port configuration, adapter discovery, protocol mode setup, blocking I/O, and
diagnostics.

Actions:

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
shrink, never grow. It currently holds 5 entries — `//src/ui/desktop:__pkg__`
and `//src/ui/desktop/biu:__pkg__` under UI, 0 in backend, plus
`//src/platform/desktop/common/serial:__pkg__` (the package itself),
`//src/platform/desktop/common/transport:__pkg__`, and `//tests:__pkg__`. The
step 5 tail's wave 7 deleted the `//src/platform/desktop/common/flash/legacy`
entry along with the package it named.

The allowlist makes this debt measurable, which the prose above cannot: each
removed entry is a layer that no longer reaches the full facade. Every entry is
a dependency step 5 (backend) or step 6 (ui) exists to remove.

Actions:

- Remove `FROZEN` entries in `scripts/check-serial-compat-allowlist.py` as the
  matching `visibility` entries are deleted; the check prints the entries to
  drop when the list shrinks.
- Treat a needed new entry as a design failure, not a paperwork step: backend or
  UI code reaching for `serial_port_actions.h` is the dependency being removed.
- Delete `serial_qt_compat` once only `//tests` remains, and fold its
  sources into the owning packages.

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
- `src/ui/desktop/dtc_operations.cpp`: 11 findings across five functions,
  suppressed when step 6e-1 touched the file — `iso15765_init` (3),
  `request_data` (2), `request_vehicle_info` (2), `request_dtc_list` (2),
  `clear_dtc` (2). All combine a `uint16_t`/`uint8_t` protocol field
  (`source_id`, `cmd`, a `QByteArray::at()` byte) with a small non-negative
  mask or constant; none is a live defect.
- `src/platform/desktop/common/serial/serial_port_actions_direct.cpp`: 8
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

Snapshot taken 2026-09-06 via `sonar list issues --project
RcusStackwalker_FastECU --statuses OPEN,CONFIRMED --format json` (paginate
with `--page`, 500/page): 3,870 open issues, all `CODE_SMELL` (no open bugs
or vulnerabilities), 2,613 Major / 901 Critical / 354 Minor / 2 Info, ~517
estimated remediation hours. No new ratchet job is needed to stop this from
growing: the project's "Sonar way" quality gate is Clean-as-You-Code and its
`new_maintainability_rating` condition already fails a PR that introduces
enough new code-smell debt (confirmed via `sonar api get
"/api/qualitygates/show?id=9&organization=rcusstackwalker"`). What follows is
a plan to pay down the *existing* backlog, ordered by risk rather than by raw
count, since count and blast radius are not the same thing here.

**Phase 1 — correctness-risk triage (highest priority, not highest count).**
`cpp:S1117` (declaration shadows an outer variable, 550 instances),
`cpp:S5276` (implicit narrowing conversion, 204), and `cpp:S5025` (raw
`new`/`delete`, 172, Critical) concentrate in the legacy per-vendor flash-op
files and `J2534_unix.cpp` — the hardware-facing layer
this document's introduction calls out as needing bench verification before
qualification. The legacy per-vendor flash-op files were deleted in wave 7
of the step 5 tail; these counts predate that deletion and have not been
rescanned since, so treat the flash-op share of them as stale rather than
current. Several `S1117` messages name variables that look
copy-paste-shadowed rather than intentionally reused (e.g. a local shadowing
`timeout_local` or `LOG_I`), which would mean the outer variable silently
never takes effect. Treat each instance as a triage question, not a
mechanical rename:

- Read every `S1117`/`S5276` instance in the top 10 offending files and
  classify it cosmetic (safe rename, explicit cast) vs. suspicious (outer
  variable's intended assignment never happens).
- For any suspicious instance, write a characterization test pinning current
  behavior before changing it — this is a behavior fix, not a style fix, and
  falls under the same TDD discipline as the flash-orchestration work above.
- Fix `S5025` instances where a `new`/`delete` pair is unambiguous and scoped
  to one function (mechanical RAII conversion); flag instances where
  ownership crosses functions or threads for the same closer review given to
  other serial/threading code in this document.
- Confirm on the next scan that new `cpp:S1117` findings no longer include
  the `emit`-signal false positives bulk-resolved above.

**Phase 2 — high-leverage Critical cleanup (mechanical, low risk).**
`cpp:S5028` (macro should be `const`/`constexpr`/an enum) accounts for 366
Critical findings, and 295 of them (80%) sit in two headers:
`J2534_tactrix_unix.h` (163) and `kernelcomms.h` (132). This is a pure type
change with no value change — the largest Critical-count reduction available
for the least risk, and a good second move once Phase 1's bug-shaped findings
are triaged out of the same neighborhood.

**Phase 3 — bulk mechanical modernization (mechanical, higher volume).**
`cpp:S6022` (use `std::byte`, 580) and `cpp:S5945` (C array →
`std::array`/`std::vector`, 197) concentrate in the same legacy per-vendor
flash-op file family (SH705x K-Line/CAN/DensoCAN/diesel siblings), which wave
7 of the step 5 tail deleted — these counts are likewise stale for that
share and unscanned since; fix both rules per file in one pass rather than
one rule across all files, so the same buffer-handling lines aren't touched
twice. `cpp:S125` (539, remove
commented-out code) has no behavior risk — fold its removal into whichever
file is already open for Phase 1/2/3 work rather than a dedicated sweep, plus
one pass on the worst offender (`mainwindow.cpp`; `ecu_operations.cpp`, formerly
the other, was dead code and was deleted in step 6c).
This is the same action already named below under "Naming and source/data
organization"; do not track it twice.

**Phase 4 — structural rules, absorbed into existing P1 items, not a new
track.** `cpp:S1820` (struct exceeds 20 fields, 26 instances) is exactly
`ConfigValuesStructure`, `LogValuesStructure`, and `EcuCalDefStructure` —
already tracked above under "P1: Replace parallel-list data models".
`cpp:S3776` (cognitive complexity, 89 instances) hits `map_edit.cpp` at the
same functions already named under "P1: Separate UI from application logic"'s
two pinned defects. Close these Sonar findings as a side effect of that
existing work instead of opening a parallel initiative:

- When resolving the parallel-list-model or `map_edit.cpp` P1 items, confirm
  the corresponding `S1820`/`S3776` instances clear as part of that change.
- The remaining scattered `S3776`/`S134` (nesting depth, 154) instances that
  don't land on an existing P1 item stay a plain backlog — re-run the `sonar
  list issues` query above when picking up unrelated work in a file to see if
  it carries one, rather than scheduling a dedicated phase for them.
- Duplication clusters a 2026-09-05 new-code scan found outside the flash
  executors, none yet extracted: the `parse_axis`/`parse_table` skeleton shared
  by `src/backend/definition/{ecuflash,romraider}_parser.cpp`; the
  dialog→validate→write tail of the two wizards in
  `src/ui/desktop/definition/definition_authoring_dialog.cpp`; and the XML-load
  preamble and `text_or_empty` helper in
  `src/backend/config/{car_model,protocol}_catalog.cpp`. The five adapters in
  `src/platform/desktop/common/transport/`, whose read/write guards differed
  only by a label string, need re-measuring.
- Test scaffolding left duplicated when `FakeCancellationToken` was adopted: a
  local `NeverCancelled` in `subaru_denso_mc68hc16y5_02_executor_test.cpp` and
  `subaru_denso_sh7055_02_executor_test.cpp`, and a family-local
  `RecordingClock` repeated across roughly ten executor tests. Move them onto
  the package-owned fakes in `src/backend/ports/testing/`.
- `WriteSelection.ReproducesTheFourSpaceQDomIndent`
  (`src/backend/logging/logger_conf_test.cpp`) pins pugixml output against
  bytes captured from the deleted `QDomDocument::save(output, 4)` writer. It
  was transitional, proving the logger-conf migration once. Replace it with a
  golden regenerated from pugixml's own output plus the existing
  `write_selection` → `read_selection` round trip; keep the four-space
  `format_indent` setting itself.

### P2: Naming and source/data organization

Some names and data placement still reflect earlier architecture:
`log_operations_ssm.cpp` contains MUT/DMA bench utilities. The duplicate
`STATUS_SUCCESS`/`STATUS_ERROR` macros are resolved: step 6e left one
definition, in `src/platform/desktop/common/serial/serial_facade_codes.h`. They
stay macros there because the Windows SDK's `ntstatus.h` defines
`STATUS_SUCCESS` as one.

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
