# Design Notes

Decisions, rejected alternatives, hardware knowledge, and gotchas from the
completed step 5 and step 6 design work that the code, the
[ADRs](adr/README.md), the [flash qualification matrix](flash-qualification-matrix.md),
and the [tech-debt roadmap](tech-debt.md) do not already record.

The design specs and implementation plans these notes were distilled from were
deleted once their work landed. They remain in git history: list them with
`git log --diff-filter=D --name-only -- docs/superpowers`.

Per-family wire behavior and every deliberate correction to legacy behavior
live in the qualification matrix, not here. The matrix is the source of truth
for "what does this family do on the wire".

## Backend ports and errors

### Silence is a value, not an error

An ECU that simply does not answer within a poll cycle is a successful value
(`PollData{responded = false}`, or an empty optional from a read), never an
`Error`. Only genuine faults become errors: disconnect, a malformed or negative
response, a timeout that is distinct from an ordinary missing frame, and
cancellation. This keeps miss-counting and reconnect logic treating
non-response as routine, and keeps `ErrorKind` meaning "something is actually
wrong". Apply the same convention to any new protocol or transport seam.

### Name a port method for each wire behavior

`IKlineFlashTransport::write_raw()` is a separate method from `write()` rather
than an `EchoPolicy` parameter or a configure-time flag. The two coincide on
J2534 but diverge on direct serial, where `write()` drains the local echo. A
defaulted parameter would let a family silently acquire the wrong echo
behavior. Likewise the adapter exposes semantic line methods
(`disable_lec_lines()`, `enable_programming_voltage_line()`,
`enable_boot_mode_lines()`) rather than raw RTS/DTR setters. When an option
changes behavior on the wire, not just configuration, add a named method.

### Every transport `configure()` clears the ISO-14230 auto-header

`SerialPortActions` is one session-lifetime object shared by every
operation, and `reset_connection()` closes the port without clearing
`add_iso14230_header`. A CAN flash that followed a K-Line session therefore
inherited the auto-header. Every transport's `configure()` clears it
explicitly and fails closed if it cannot.

## Flash architecture

### Correct defects, preserve the wire

No drained family has hardware qualification to catch a regression, and a
wire change cannot be reviewed alongside a structural migration, so early
waves preserved legacy behavior by default and named each divergence in the
matrix. Wave 5 set an evidence rule for corrections: local evidence of a
defect (an out-of-bounds access, a malformed frame, an impossible condition,
an ignored flag, or a contradiction with a local definition), a focused test
that reproduces it, byte- or outcome-exact tests pinning the new behavior,
and a matrix note citing the legacy behavior. Similarity to a sibling family
is grounds to investigate, never authority to change addresses, timeouts,
retry counts, response tolerance, seed logic or ordering.

From wave 6a-3 onwards each family was migrated under a standing exception,
approved per family: command bytes, sequencing and timing budgets are
preserved exactly, but bounds, read-integrity, reply-gating and cancellation
defects are corrected, so a malformed reply or a cancellation stops every
later command instead of reporting success. Use this exception for any future
migration of legacy behavior that lacks hardware qualification.

### Operator steps never block an executor

A portable executor is synchronous and dialog-free; `IEventSink` carries logs,
progress and notices and must never solicit or wait for an answer. Two
alternatives were rejected for workflows that need a decision part-way
through: a blocking prompt callback inside `execute()` (it re-invents a
blocking queued connection and makes shutdown unbounded) and a resumable
executor with serialized continuation state (machinery only one workflow
needed). Events are advisory and never change control flow or report
completion: the returned `Result` is the single authoritative outcome, so the
UI shows at most one terminal failure, chosen by `ErrorKind`, and a
user-initiated `Cancelled` shows no failure dialog. Instead of prompting from
inside an executor:

1. **Permission before starting** is a `ConfirmationSpec` collected up front;
   its presence in the plan means "granted" (the Unisia Jecs M32R "apply VPP"
   prompt, the Denso MC68HC16Y5 BDM kernel bootstrap, the Hitachi SH7058
   K-Line read).
2. **A decision between bounded attempts** is a workflow prompt between two
   `FlashAttempt`s. The workflow's `next()`/`submit()` step machine supports any
   number of attempts. The EEPROM inspect-then-retry loop and the Unisia Jecs
   M32R bootmode "remove MOD1" step both use it. For a single legacy operation,
   split it this way only where the wire sequence already has a real
   connection boundary at that point (bootmode's legacy code called
   `reset_connection()` there anyway), not merely to route around a
   synchronous executor.

### EEPROM reads try modes 2, 3, 4 in that order

The Denso SH705x EEPROM families try read mode 2, then mode 3 only if the
operator discards the previewed bytes, then mode 4: never more attempts, never
out of order. The operator cycles ignition between modes; declining that prompt
ends the session as a cancellation, not a failure. Write and test-write are
permanently `Unsupported`, because the legacy write call was already dead code.
Preserve the ordering and the ignition gate in any change to these workflows.

### Keep `FlashOperation` to Read, TestWrite and Write

When a family's "Write" is structurally different — the Denso MC68HC16Y5 BDM
family uploads a kernel to RAM and jumps to it without touching the ROM — do
not add a `FlashOperation` value. Every plan, validator and workflow switches
on that enum and no other family needs the shape. Carry the divergent payload
in existing plan fields (BDM and the Unisia Jecs M32R bootmode kernel attempt
both carry the kernel as the plan image) and make the difference explicit to
the operator with a dedicated `ConfirmationSpec` (BDM's `KernelBootstrap`).

### No shared helper for the Hitachi and Mitsubishi M32R K-Line families

The two families share SSM byte framing, but a common helper was rejected.
Mitsubishi requires its initial handshake with one timeout policy; Hitachi uses
optional probes, a short recovery-wake timeout, fallback parsing and tolerant
write acknowledgements. A helper would have to expose timeout and tolerance
policy at its boundary, recreating the configurable state machine that
[port-then-factor](#where-port-then-factor-shared-code-and-where-it-did-not) exists to avoid.

### Where port-then-factor shared code, and where it did not

Do not force all ECU families into one configurable state machine. Handshake
order, service identifiers, field offsets, erase behavior and recovery
semantics differ across K-Line/CAN and Denso/Hitachi/Mitsubishi families. The
rule is an ordering, not a judgment call: within a clone cluster, port each
family to a tested portable executor first, and only then factor out what is
provably identical between the tested executors. Extraction never happens
against untested Qt sources. Share pure byte algorithms, framing, validation
primitives, block planning and workflow plumbing; keep protocol sequence and
safety policy readable inside each verified family. Transfer loops stay
family-specific until byte-level tests show that erase rules, address
translation, response opcodes and retry semantics are identical. New or
modified families take protocol sessions through `IKlineTransport`,
`ICanTransport` or `ISsmTransport`, so handshake, timeout, rejection and
cancellation paths are scriptable.

Shared today, and not open extraction work:

- The [SSM protocol core](../src/algorithms/protocol/ssm/ssm_protocol_core.h):
  SSM headers and checksums, seed-key and payload transforms, the non-standard
  CRC, frame validation and byte formatting. Do not reintroduce per-module
  copies. Family-specific lookup tables stay at the call sites because they
  are protocol data, not duplicate algorithms.
- `src/backend/flash/flash_utils.*`: byte stuffing and ISO-15765 flash setup.
- `src/algorithms/protocol/bytes.h` and
  `src/platform/desktop/common/bytes/qt_bytes.h`: portable byte types and
  explicit desktop Qt conversions.
- `FlashDialog` and `FlashWorkflow`: logging signals, prompts, progress,
  cancellation and worker-thread plumbing for every family.
- `FlashAttemptOutcome` (private to `flash_workflow.cpp`): the
  terminal/outcome/bytes/rom_id/failure bookkeeping shared by all
  `FlashWorkflow` subclasses.

Still-open opportunities, to be taken only with scripted tests first:
family-aware response validators consuming `bytes::ByteView` and returning a
structured result (never logging or choosing retry policy), and pure
block-planning helpers.

Where sharing happened, and where it did not:

- Wave 3 produced `subaru_tcu_cvt_mitsu_can_common` for the MH8111 and MH8104
  TCU families.
- Wave 4 found none of its cluster's protocol functions identical across all
  four families and shared only crypto key tables
  (`denso_iso15765_can_common.h`); wave 5's ISO-15765 families reuse them for
  stock security and kernel upload only.
- Wave 6b-2 shared only byte-identical seed and encrypt tables with the EEPROM
  K-Line executor (`denso_sh705x_kline_common.h`).
- Waves 1 and 6 declined any common code.
- `single_window_plan` (`src/backend/flash/ecu/single_window_plan.*`) covers
  declarative plan validation for ten kernel-free families, those described
  entirely by a protocol id, an MCU, a read window, a write window and one
  image size. It shares plan validation only; the executors stay un-factored
  because their look-alike blocks differ in timeouts, retry counts and response
  strictness. It was deliberately not widened to wave 5's kernel-backed,
  16-block families.

Each shared header names its consumers and what was compared.

## Logging

### The logging loop lives in the backend

Two alternatives to the portable, synchronous `LoggingUseCase::run()` were
rejected. A platform-owned loop, with Qt holding the miss-counter and reconnect
policy over a backend `start`/`poll_once`/`stop`, would make a future Kotlin
port duplicate that policy. A callback-driven portable state machine fed by
platform I/O results was scheduling machinery with no consumer. All workflow
policy — miss counting, reconnect cadence, status transitions — stays inside
the bounded `run()`; the platform only supplies a thread and forwards events.

### SSM raw samples concatenate decimal byte spellings on purpose

SSM samples build their raw value by concatenating each response byte's
decimal spelling (bytes `0x01, 0x02` become `"12"`), while MUT/DMA and CDBG
spell the decoded unsigned integer in decimal
(`RawAssembly::DecimalBytesConcatenated` versus `UnsignedIntegerDecimal`). It
looks like a bug, but it is the exact input every shipped RomRaider conversion
expression was written against. Do not normalize it without auditing every
shipped expression.

### Logger ownership and stable identities

The portable `LoggerModel` holds logger definitions, selection and support.
`DesktopComposition` owns it and `LoggerDefinitionService`; `MainWindowServices`
passes references to the GUI. The definition installs once and is exposed only
as const data. XML `enabled` defaults stay immutable. Operator selection, ECU
support and the desktop display cache have separate owners. Selection reads
and edits never restore XML support flags. Parameters and switches use separate
(protocol, ID) namespaces; defaults and presentation retain definition order.
Startup chooses the first 15 gauges, 12 digital values and 20 switches without
filtering support; missing-ECU defaults use current support at those limits.

Capability application preserves the legacy asymmetry: unavailable parameter
bytes disable parameters, unavailable switch bytes retain previous flags.
Identification without capability bytes never calls capability application.
Unreadable selection files leave choices alone; successful reads clear selected
IDs before resolving an entry. Without parameter definitions an absent ECU is
never initialized. Failed saves retain operator edits, and definition failures
remain nonfatal. Unresolved IDs survive selection round trips, are omitted from
displays and retain their column positions as empty CSV cells.

Per-run snapshots capture the protocol, stable identities, support, conversion
and target as owned values. SSM still polls disabled channels at their original
lower-panel offsets and concatenates decimal byte spellings; MUT/DMA filters
unsupported channels; CDBG does not filter on support. Only the first conversion
is used, with fixed decimal display formatting. Later selection edits never
change the worker's session. CSV retains its file lifetime, schema, trailing
commas, column order and numeric formatting; it resolves the current selection
in the active run's captured protocol.

Two deliberate corrections are qualified by synthetic automated evidence:

- **Chooser identity:** the legacy slot resolved labels and selected the last
  matching definition when two labels were identical. Items now carry protocol
  and ID, for gauges, digital values and switches. The duplicate-label fixture
  first reproduced that behavior on 6l-1; the corrected fixture asserts the exact
  selected ID in every panel. A mutation that resolves to the last item fails
  all three cases.
- **CSV protocol identity:** the legacy `indexOf(id)` selected the first ID in
  any protocol. A shared-ID fixture reproduced the wrong SSM column for CDBG.
  CSV now resolves within the captured run protocol even if the UI protocol
  changes. Exact header/value assertions also cover unresolved cells. A mutation
  choosing the definition's first protocol fails the fixture.

The [logging composition bench checklist](logging-composition-bench-checklist.md)
remains the hardware qualification gate. These corrections and the migration
have automated coverage only; they are not ECU/TCU bench-qualified. Android,
live reconfiguration, CDBG wire changes and calibration migration are outside 6l.

### pugixml indents with a tab by default

The `QDomDocument::save(output, 4)` writer that pugixml replaced used four
spaces. Any pugixml writer for a file users already have on disk must set the
indent explicitly (as `logger_conf.cpp` does), or every existing file
reformats wholesale on the next write.

## Definitions

### pugixml is an adjudicated dependency

pugixml (MIT, no further dependencies) was adopted once, deliberately, as the
shared portable XML primitive for definitions, logger definitions and logger
conf. Backend targets depend on it directly, with no wrapper. It is the
project's only adjudicated third-party runtime library (the first, OpenSSL,
was removed in #216). Any further one should get the same explicit
adjudication rather than arriving with whichever change first needs it.

### Single-consumer wizards move to the UI instead of behind a port

The former `FileActions::create_new_definition_for_rom` and
`use_existing_definition_for_rom` wizards moved wholesale into
`DefinitionAuthoringDialog` (`src/ui/desktop/definition/`) rather than behind
an `IDefinitionPrompt` port:
the flow is presentation end to end with exactly one caller, so a port would
only wrap that caller. The non-UI remainder (`apply_missing_definition_defaults`)
stayed in the backend. Precedent: a single-consumer UI flow moves to `src/ui`;
anything a second caller will need gets a port.

### Desktop catalog lookup and authoring ownership

A composition-owned `DefinitionCatalogSession` in the desktop platform layer
replaced the former `FileActions` and its parallel definition indexes.
It implements the existing portable `IDefinitionCatalogs` interface and shares
one `DefinitionService` with ROM opening. The dialogs retain operator decisions;
the session records authored destinations only after successful writes.

Startup lookup provenance and fresh catalogs have different lifetimes. A fresh
scan does not replace the retained lookup: a file deleted after startup still
needs to produce the established ROM-open notice. Explicit refresh replaces
one format's lookup only on success; empty configured sources retain it,
while a successful scan that skips every unusable file clears it.

Retained lookup uses ordered typed ID/source records with first-match behavior.
It deliberately does not use `DefinitionCatalog`, whose validation rejects
conflicting duplicate identities. Successful authoring appends a lookup record
and remembers its destination for discovery outside the configured directory.
Changing directories drops prior discovery on refresh, preserving authored
handles. No new parser, portable port, worker, or backend policy is introduced.

Composition now links configuration and kernel resource registration explicitly;
it previously obtained both through `FileActions`. Test composition supplies
the same registration directly. The workspace and ROM opener stop before the
catalog session, which stops before its borrowed service and configuration.
This structural migration establishes no new hardware qualification.

## Calibration

### Files edited before PR #274 may hold wrong bytes

Before #274 the calibration write path byte-swapped signed multi-byte reads and
read signed 24-bit values as zero, wrote bytes in the opposite order to the
element's declared endianness, and wrote float-storage edits as the bit pattern
of an integer parse rather than the value's IEEE-754 bits. A calibration file
edited with an older build on a signed multi-byte, int24 or float-storage map
may hold bytes that differ from what its grid showed at the time. Current
builds decode those bytes correctly rather than reproducing the old error, so
re-check such a file against its definition before flashing it.

### Calibration defect letters

Code, tests and the tech-debt roadmap cite step 6b's write-path defects by
letter:

- (a) the `wrx02` read/write address predicates disagree — open; see the
  [tech-debt roadmap](tech-debt.md).
- (b) the edit path ignored `startpos`/`interval` striding.
- (c) bounds enforcement differed per operation. `encode_guarded` applies the
  range check only; `apply_increment` alone keeps legacy's sign-wrap
  heuristic, because it reverts a single cell rather than failing the edit.
- (d) a zero increment looped the modal dialog.
- (e) interpolation used a 128×128 stack array.
- (f) signed multi-byte reads were byte-swapped and int24 read as zero.
- (g) write byte order was inverted relative to the endian label.
- (h) float-storage writes stored integer bit patterns.

(b) through (h) are fixed; (f) through (h) landed in #274.

### Calibration session ownership and operation images

The desktop composition owns `CalibrationWorkspace` and outlives its windows.
Each calibration has a stable `SessionId`, never reused within the workspace;
files-tree rows, map windows and UI view state use that ID instead of a slot
position. Closing one ROM does not renumber another. A stale ID resolves to
nothing, so delayed UI actions cannot edit a different session. Close removes
the session's map windows and view state together; map color ranges belong to
the open map view and must not outlive its session. Expanded categories, open
maps and missing-definition placeholders are UI state, separate from ROM data.

The session owns the ROM bytes, optional resolved definition and protocol
metadata. ROM bytes are the only truth for values: map windows decode on
demand, edits call `write_bytes`, and views re-decode. The final consumer slice
reads map metadata directly from the typed definition too, instead of retaining
a read-only legacy projection. Definition-less ROMs remain modeled sessions.
Hex display receives its own snapshot rather than retaining calibration data.

Checksum correction works on a temporary operation image for save or write.
The corrected image reaches the repository or flash request; completion,
failure and cancellation leave editable session bytes unchanged. A successful
`RomSaveUseCase` write updates the source path and basename (with `default.bin`
when no basename exists), preserves the file/ECU-read origin and clears dirty.
Failure preserves the source and dirty state and emits the existing error log
and operator notice. A subsequent edit marks the session dirty again. Checksum
warnings and operator confirmations remain in the UI.

Open retains the legacy primary/secondary definition precedence and reported
ROM-ID fallback, vehicle selection, unpadded file-size label and subsequent
flash-method padding. A size-validation failure retains the definition header
with no maps; an unreadable indexed definition opens without a definition and
keeps its load-failure notice. Create/import writes a definition and does not
reopen the definition-less session. Failed or cancelled ECU reads create no
session; successful reads are adopted after dispatch completes.

The [calibration defect letters](#calibration-defect-letters) still apply,
including the open `wrx02` predicate mismatch. Post-read adoption and
checksum/save/write paths require bench re-verification before release;
automated coverage does not establish hardware qualification.

### Preflight cancellation is not correction cancellation

`CalibrationOperationCoordinator` sequences write preparation and Save/Save
As in the UI layer, with a Qt-free closure; its dialogs go through
`ICalibrationInteraction`, which `QtCalibrationInteraction` renders.
`MainWindow` supplies the selected session, applies the status-label and
files-tree updates, and keeps the port checks, cleanup guard, battery polling
and flash dispatch. Two operator answers look alike but have different
consequences, and both are compatibility requirements:

- Cancelling the missing-checksum-module warning that precedes Write or Test
  Write (the selected vehicle's checksum flag is `n/a`) stops the operation.
  Nothing is refreshed, corrected or dispatched; the
  [cleanup guard](#start_ecu_operations-cleanup-is-a-scope-guard) still runs.
- Declining checksum correction, an unknown MCU, or a correction outcome
  without bytes leaves the operation image unchanged and lets the write or
  save continue with those bytes. The "Checksum calculation canceled!" log
  belongs only to the declined missing-module case.

Save As corrects before opening its picker; a dismissed picker or empty
filename writes nothing. A successful Save As relabels the files-tree row of
the session resolved before the picker opened, not the row selected
afterwards.

Test ownership follows the split. `calibration_operation_coordinator_test`
(no Qt, a scripted interaction mock and the real `RomSaveUseCase`) owns the
sequencing, logs, metadata refresh, filename normalization and persistence
invariants; `qt_calibration_interaction_test` owns dialog text and wiring.
`test_mainwindow` keeps the window's wiring: cleanup after a rejected
preflight for both write commands, the status label, log forwarding to
`LogChannel`, backend-corrected bytes reaching the saved file, and Save As
relabelling across a selection change.

## Build and guards

### Keep portable targets out of packages with a legacy glob

A `glob()`-based target silently absorbs any new source dropped into its
package, and a new portable target's glob can equally pull in a legacy Qt
file. Land portable code in its own package, and use explicit `srcs` lists for
any target sharing a package with legacy code.

### A glob fails when any one pattern matches nothing

Bazel fails a `glob()` if any single pattern matches no files, unless
`allow_empty = True` is set — not only when the whole result is empty. Deleting
the last file a pattern covers, for example mid-way through a migration, needs
`allow_empty` until the target itself goes.

## Desktop composition root

### The composition outlives the window it serves

`apps/desktop/main.cpp` declares `DesktopComposition` before `MainWindow`,
inside the `RESTART_CODE` loop: every restart rebuilds both, and
`MainWindow`'s `MainWindowServices` references stay valid until the window
is gone. The composition's destructor releases dependents first — logging
engine, remote utility, serial facade — then quits and joins the syslogger
thread. Before step 6c that thread was never stopped (`SystemLogger::finished`
is never emitted), so every restart leaked one.

### Construct the serial facade through `desktop_serial_factory`

`apps/desktop` must not depend on the facade target directly. The factory
exposes construction only, returns an owner with a custom deleter, and keeps
`SerialPortActions` an incomplete type in `apps/desktop`. It connects
the facade's `LOG_*` signals to the sink with string-based `SIGNAL`/`SLOT`,
which fail only at runtime; `desktop_serial_factory_test` is what catches a
broken one.

## Logging composition

The MUT/DMA, CDBG, and SSM registrations live in
`register_desktop_logging_protocols`, called once by `DesktopComposition`
before constructing the window. The helper is a separate
`//src/platform/desktop/common/transport:logging_protocol_registration`
target, visible to the composition root and its own package. The transport
package already has permission to use `serial_qt_compat`; neither the
composition root nor the logging runtime gains direct facade access.

Registration performs no adapter I/O. The engine still invokes factories
synchronously during `start()`, before launching the existing worker. CDBG
keeps its seven ordered settings, first-failure return, and short-circuited
port-open check. MUT/DMA keeps `AlreadyInMode(125000)`. Factories retain the
serial facade and clock by reference; the composition destroys the engine
before either service, including during application restart.

`DesktopLoggingSnapshot::target_is_ecu` captures the radio-button selection
after snapshot validation and before the UI copy and engine start. SSM uses
that per-run value and queries the adapter capability when its factory runs.
Factories never read widgets. `MainWindowServices` no longer exposes the
logging clock, and `MainWindow::setupLoggingEngine()` only wires signals.
The UI still owns protocol/policy selection, connection orchestration, and
log-file handling. Its transport dependency remains for the standalone
MUT memory helpers in `log_operations_ssm.cpp`.

Factory tests use the real serial facade with a fake backend, including CDBG
setup failures and handshake, SSM target/adapter variants and nonsequential
sample offsets, and MUT initialization/channel bytes. SSM/MUT factories are
invoked synchronously through test-local access; engine lifecycle tests and
CDBG's public-engine startup test cover worker integration. The composition
registration test inspects keys without opening hardware, and UI regressions
cover per-run selection and preservation of injected factories.

Hardware qualification is tracked separately in the
[logging-composition bench checklist](logging-composition-bench-checklist.md).

## Diagnostic tools

### The port is byte-faithful; `uses_j2534()` is not hidden

`IDiagnosticLink` does not frame, unframe, or add headers or checksums --
that stays with the facade (when a header flag is set) or with the caller.
The one place the adapter split is genuinely load-bearing is the five-baud
response check and the `read` vs. `read_obd` choice, both of which differ
between J2534 (OpenPort) and the direct serial backend. Rather than have the
adapter guess or the session carry two near-identical code paths, the port
exposes `uses_j2534()` and lets `DtcRun` branch on it explicitly, exactly as
`dtc_operations.cpp` branched on `get_use_openport2_adapter()` before this
step. Only the P1-max mechanism (`set_j2534_ioctl` vs. `set_kline_timings`)
is hidden inside `set_p1_max`, because both call sites want the same effect
and neither caller needs to know which one ran.

### `SerialDiagnosticLink` lives in its own platform package

The `//src/platform/desktop/common/diagnostics` package reaches the facade
through `//src/platform/desktop/common/serial/facade:serial_port_actions` as an
`implementation_deps` edge, like the other platform adapters.
`serial_diagnostic_link.h` forward-declares `SerialPortActions`, so
including it does not carry `serial_port_actions.h` to the UI; `MainWindow`
constructs a `SerialDiagnosticLink` from the facade pointer it already holds
and hands the dialogs an `IDiagnosticLink&`. This is also where `DtcWorker`
lives: it is a Qt adapter with no `SerialPortActions` include of its own.

### BIU and DataTerminal stay synchronous; DTC does not

BIU's `send_biu_msg` and DataTerminal's per-line send are each one
write-and-read triggered directly by a user action (or, for BIU's
keep-alive, a `QTimer` tick) with a bounded read timeout (800 ms, 200 ms).
Blocking the UI thread for that long is the same behavior these dialogs had
before this step, and moving either onto a worker would add thread-safety
work -- particularly for BIU's timer-driven keep-alive -- disproportionate
to a delay already this short. A DTC run is different in kind: five-baud or
fast init, up to seven supported-PID pages, six vehicle-info requests, and a
stored/pending DTC read, with 250-500 ms sleeps between most of them, adds
up to several seconds with the dialog frozen throughout. That is what
`DtcWorker` exists to fix, mirroring the `ServiceFunctionWorker` precedent
from step 5's service-functions ports.

### Pinned quirks

Four behaviors look wrong but are deliberately unchanged, each pinned by a
test rather than fixed, because fixing any of them needs a bench capture to
confirm what the ECU actually expects:

- **The OpenPort five-baud response check compares ASCII, not values, and at
  different offsets.** `five_baud_header`'s J2534 branch checks response
  bytes `[5]`/`[7]` (iso9141) and `[8]`/`[9]` (iso14230) against the ASCII
  characters `'8'`/`'8'` and `'8'`/`'f'`, while the direct-serial branch
  checks bytes `[1]`/`[2]` (iso9141) against the numeric values `0x08`/`0x08`
  and just byte `[2]` (iso14230) against `0x8F`. Whether the OpenPort
  firmware genuinely echoes ASCII digits here, or the original code meant the
  numeric comparison and got it wrong, is not something to guess at from the
  source.
- **K-Line unframing keeps its length heuristics.** `unframe_data_response`
  strips a fixed number of leading bytes chosen by the frame's total length
  (`< 7` keeps the last byte, `< 10` drops 5, otherwise drops 6) rather than
  by parsing a length field; `unframe_dtc_list_response` uses a coarser
  two-tier version of the same idea (`< 7` keeps the last byte, otherwise
  drops 4). Both reproduce today's behavior exactly; a proper length-field
  parse is a separate, riskier change.
- **DataTerminal's `delay(...)` parse yields 0.** `split(")").at(1).split("(").at(0)`
  parses `delay(100)` to an empty string, so every scripted delay is 0 ms.
  Scripts written against the existing (broken) timing would behave
  differently if this were fixed incidentally.
- **The ISO-15765 init NRC is described from offset 3, not 4.** Every other
  NRC in the DTC session (`request`, `clear_dtcs`) is described from the
  frame's own response index (4 for iso15765). `can_init` describes the NRC
  starting one byte earlier, at offset 3, reproducing today's off-by-one
  exactly rather than aligning it with the others.

### Behavior changes

These differences from the original dialogs are listed as bench rows in the
[diagnostics checklist](diagnostics-bench-checklist.md); the ones with no bench
row are automated-only.

- DTC runs off the UI thread. Cancellation is best-effort: `DtcRun` checks it
  only inside `IClock::sleep` and `IDiagnosticLink::read`/`read_obd`, so an
  already-cancelled run still costs one `open()` and one init exchange, and
  closing the dialog waits for the worker to stop.
- Short init or request responses fail as an init failure or `BadResponse`
  through `obd_frames.h`'s bounds-checked helpers (automated only).
- DataTerminal resets and sets every flag before opening, so it no longer
  inherits header, CAN or 29-bit flags an earlier tool left set.
- A failed DTC run always logs one error line. On iso14230 a failed `open()`
  during fast init is treated like any other fast-init failure, so
  `DtcRun::init()` still falls back to five-baud; the run ends `Disconnected`
  only if the five-baud path's own re-open also fails.
- PID pages `0x81`-`0xE0` are accepted: legacy compared a signed `char` against
  the `uint8_t` PID, so echoes for `0x80`, `0xA0` and `0xC0` never matched.
- Each DTC run starts clean; legacy never reset `fast_init_ok`.
- `vehicle_info` logs `"Supported PIDs: "` as one `IEventSink::log` call. It
  renders identically.

## Flash-operation dispatch

### `start_ecu_operations` cleanup is a scope guard

The idle-line serial reset, battery-polling stop, and log-transport
re-selection run from a `qScopeGuard` constructed right after the port check.
Every exit from that point runs them. Before step 6d the Denso TCU service
path reached them through a `goto`, and the write-preflight early returns
("No file selected!", Cancel on the checksum warning) skipped them, leaving
battery polling running.

### A read enters the workspace only after success

Since step 6m, the post-read handoff adopts an image into the calibration
workspace only after a successful read with data. Cancellation, failure,
`Unsupported` and handled TCU service actions leave the workspace unchanged.
There is no preallocated calibration slot to release. Post-read handoff still
requires bench re-verification before release.

### `reset_serial_to_idle` lives beside the facade

It calls `SerialPortActions` methods, so it needs the facade's definition,
which the UI package that calls it must not see. It lives in the serial
package as `serial_idle`, visible only to `//src/ui/desktop`. `FlashOperationController` only passes
the facade pointer through, so it needs no serial edge at all.

## Platform selection

### The composition root owns the direct/remote rule

`SerialPortActions` receives a backend factory and never learns which
backend it drives. `serial_connection_from_args` (`apps/desktop`) maps an
empty `--host` to `DirectSerial`, and `make_serial_backend_factory`
(`desktop_serial_factory`) builds the matching backend. Before step 6e the
rule lived in the facade's constructor and again in the CAN transport
factory; now a new platform, such as step 7's Android build, supplies its
own factory without touching the facade.

### Per-OS hooks sit behind one guard-free header

Each former `Q_OS_*` branch in the direct backend is a protected member
declared once in `direct/serial_port_actions_direct.h` and defined in exactly
one of `direct/unix/serial_port_actions_direct_unix.cpp` /
`direct/windows/serial_port_actions_direct_windows.cpp`. Hook bodies were
moved verbatim, so a Windows regression shows up as a compile failure in
the wrong file rather than as changed behavior. Both J2534 packages publish
their API at `src/platform/desktop/j2534/j2534_api.h`, so the common code
has no guarded include.

### The binary names the platform

`desktop_serial_factory` links only the declaration of
`make_direct_serial_backend()`. `fastecu` and `fastecu-bench` each carry a
`select()` alias picking `direct/unix:serial_port_actions_direct_unix` or
`direct/windows:serial_port_actions_direct_windows`; each hook library carries
a dependency keep on the shared `direct/common` implementation. Tests use
`direct_serial_backend_for_tests`, a narrow keep beside the header-owning
library they include. A target that forgets the implementation fails to link.

### `STATUS_*` stay macros

`serial_facade_codes.h` keeps `STATUS_SUCCESS`/`STATUS_ERROR` and
`SERIAL_P*` as macros: the Windows SDK's `ntstatus.h` defines
`STATUS_SUCCESS` as a macro, which would break a constexpr of that name.

### One header cannot be moc'd by two targets

`qt_cc_library`'s moc genrule names its generated file after the header's
basename alone (`third_party/qt/qt.bzl`), not after the calling target, so
the per-OS hook libraries cannot both list `serial_port_actions_direct.h` in
`hdrs` directly — the genrules would collide at load time, on every
platform, regardless of `target_compatible_with`. The header-only
`direct:serial_port_actions_direct` library mocs it once; the shared
implementation and both per-OS hook libraries depend on it instead of
moc'ing the header themselves, and `direct_serial_backend_for_tests` is an
alias to whichever hook library matches the host OS. This is a Bazel/Qt integration limit, not a design preference — a
future header split into per-OS pieces should keep it in mind.

## Connection and identification

### `AdapterConnection` is a concrete adapter, not a port

`MainWindow`'s connection handling -- port listing, opening, the
log-transport flag profile, idle resets, battery voltage -- is desktop-facade
detail: port-name strings, OpenPort-only voltage, remote-replica waits.
Android's first target, MUT/DMA logging over USB, would reuse almost none of
it. So `//src/platform/desktop/common/connection:adapter_connection` is a
concrete Qt class `MainWindow` calls directly, and each member reproduces the
facade call sequence it replaced, pinned by `adapter_connection_test`. The
portable part is the protocol: `identify_ssm_ecu` in `//src/backend/diagnostics`.
If a later platform needs connection lifecycle, a port can be cut from the
adapter's surface then.

### The two idle resets stay separate

`clear_link_flags` (a port refresh) clears every link flag; `return_to_idle`
(a disconnect) resets baud and parity and leaves the flags alone.
`connect_to_ecu` does not reapply the flags `log_transport_changed` set, so a
disconnect that cleared them would make the next connect on a CAN transport
open as K-Line. A port refresh still leaves parity as it found it -- pinned,
not fixed.

### SSM2 identification is validated the way RomRaider validates it

RomRaider's `SSMResponseProcessor.validateResponse` is the reference: header
`0x80`, tester `0xF0`, the requested target, response code `0xFF`, and the
checksum. The legacy code checked only the length, so a corrupted frame
became a wrong ECU ID and wrong log capabilities. The frame is first cut to
its declared length, because the legacy code accepted trailing bytes.
SSM1 keeps its length-only check: RomRaider has no SSM1, and its checks are
SSM2's.

### There is no write that skips the echo check

`IDiagnosticLink` has one write, echo-checked. SSM1 was the only caller of a
plain `write_serial_data`, for two of its three wake-up writes; they are
echo-checked now. RomRaider has no such write either -- it strips the K-Line
echo from the response by length. This is a bench-gated behavior change.

### Connect is asynchronous

`ConnectionCoordinator` owns the state machine described here: the generation
fence, the per-attempt continuation and the lock/unlock sequencing. It is
Qt-free; `QtIdentifyLauncher` wraps the worker and `MainWindow`'s nested
`ConnectionPresentation` locks the controls and shows results. `MainWindow`
keeps the port-open preamble, disconnect, port refresh and the logging
continuation, and calls `cancel()` at every entry point that touches the
facade.

`SsmIdentifyWorker` runs the five-attempt loop off the UI thread. The facade
is used by one party at a time, so the UI keeps out of it while the worker
runs:

- Connect first stops an active logging session. `LoggingEngine::stop()`
  joins synchronously, so the logging worker is off the facade before
  identification starts, and the Logging action unchecks.
- While identification runs, the log-transport combo, the ECU/TCU radio
  buttons, and the Connect and Logging actions (with their shortcuts and
  toolbar icon, which share the same `QAction`) are disabled. The radio
  buttons stay frozen through capability parsing and the logging
  continuation, so the logging snapshot records the target that was
  identified.
- Battery sampling skips its facade read while identification runs.
- Every other entry point that touches the facade stops identification
  first: disconnect, a port refresh, opening a port, a log-transport change,
  the DTC, BIU, and terminal windows, flash operations, and window teardown.
  A cancelled identification unlocks the port selector the way Disconnect
  does, though the port stays open.

A cancelled connect tells its caller so: a pending Start logging reports
"Unable to connect to ECU". That `on_done(false)` runs synchronously inside
`stop_identification`, before the cancelling caller uses the facade, so a
continuation must not start a connection synchronously (the contract is
stated at `connect_to_ecu`).

Completions are fenced per attempt. A completion queued by a worker that was
stopped is recognised by a generation counter and dropped, because Qt still
delivers an event posted before its sender died. Each attempt owns its
continuation: capability parsing can open a notice whose nested event loop
starts or stops another identification, and that turns the outer attempt's
result into "not connected" instead of continuing its logging start on the
newer attempt's state. A connect still counts as successful when the port
opened and identification failed, as before.

### Pinned quirks

- A connect succeeds when the port opens, even if identification fails.
- Raw-CAN identification does nothing: its legacy branch tested a protocol
  value no configuration sets.
- SSM1 is checked by length only.
- CAN identification uses UDS `22 F1 82`, not SSM `AA`, so CAN logging gets
  no capability bits (see the [tech-debt roadmap](tech-debt.md)).
- `log_transport_changed` sets 29-bit identifiers for iso15765 and 11-bit for
  raw CAN.
- A port refresh leaves parity unchanged.

### Behavior changes

Bench rows for these are in the
[connection checklist](connection-bench-checklist.md); the malformed-frame
items are automated-only.

- The developer toggles `can_listener`, `simulate_obd` and
  `test_haltech_ic7_display` are deleted; none was in the shipped menu
  and each looped forever on the UI thread.
- SSM identification runs off the UI thread and can be cancelled. A port
  refresh, opening a port, a log-transport change, the DTC, BIU and terminal
  windows, and a flash operation cancel it; a pending Start logging then
  reports "Unable to connect to ECU", the port stays open with no ECU
  identified, and the port selector is unlocked. The ECU/TCU radio buttons
  and battery-voltage sampling are suspended while it runs.
- SSM2 init responses are validated and trailing bytes dropped. Short or
  malformed init frames fail as `BadResponse`. SSM1's trailing drain stops
  after 100 reads and a trailing frame too short for an ID is ignored.
- SSM1's two plain writes are echo-checked.
- K-Line SSM2 identification opens the link itself with the settings it used
  to inherit, and every K-Line `IDiagnosticLink::open` sets parity explicitly.
- BIU opens directly at 10400 instead of opening and then changing speed; it
  still saves the port it opened as the configured port.
- A non-Subaru connect, including every Mitsubishi MUT/DMA logging start,
  skips 2.5 s of empty retries.
- With no serial port present, the DTC, BIU and terminal commands warn
  instead of indexing past the end of the port list.
- Connect stops an active logging session, including MUT/DMA, before it opens
  the port. As with every session end outside the Logging toggle, an open
  `datalog_file` is not closed and the next session appends to it; it closes
  when Logging or "Log to file" is switched off, or when a flash operation
  starts.
- A successful iso15765 identification sets the ECU ID.

## Serial facade retirement

### Visibility and `implementation_deps` keep facade headers out of the UI

`serial_port_actions` is visible only to `//src/platform/desktop:__subpackages__`
and `//tests`. Every adapter whose public header forward-declares
`SerialPortActions` takes it through `implementation_deps`, so Bazel still
links the facade into dependents but its headers are not inputs to their
compiles. Visibility alone would not cover this transitive reach.

### Windows does not enforce it

The sandbox is what turns a missing header input into "file not found".
Windows builds are unsandboxed, so there the header is on disk under the
execroot and a UI include of it would compile. Linux and macOS CI catch the
same include, so the check holds across the three-platform matrix, not on
any one Windows machine.

### UI tests still see the facade headers

`connection/testing:adapter_connection_harness` hands UI tests a
`FakeBackend`, which derives from `SerialPortActionsDirect`, so testonly UI
targets receive `serial_backend.h` and its siblings. Tests need the concrete
fake to set expectations; production targets do not reach it.

### `websocket_io` is separate from the remote backend

`remote_utility.h` needs `websocketiodevice.h` and `qtrohelper.hpp` and took
them from `remote_serial_backend`, whose public header includes
`serial_backend.h`. Through the GRANDFATHERED UI edge to `remote_utility`
that was the one remaining path from production UI code to a facade header.
The two files are now `serial/websocket:websocket_io` and `serial:qtrohelper`,
and `remote_utility.h` includes them by full path: the bare spellings had resolved only through
`serial_replicas`' `includes = ["."]`.

### A probe include goes last in the file

To show that a UI source cannot include a facade header, append the include
to the end of the source rather than the top. At the top,
`serial_facade_codes.h`'s `STATUS_SUCCESS` macro collides with an enum in
`definition_file_convert.h`, and the build fails for that reason instead.

## UI channels

### The UI owns the channels

`LogChannel` and `RemotePeer` live in `//src/ui/desktop/channels` and hold
only signals. The UI states what it needs; `DesktopComposition` decides what
answers it. That keeps `SystemLogger` and `RemoteUtility` platform-only
without cutting portable ports for them: Android's first target needs
neither, and `IEventSink::log` has no timestamp or linefeed flags, which the
UI's `LOG_*` call sites rely on.

### The `LOG_*` names are a contract

`SystemLogger::log_messages` reads a line's level from the name of the
signal that delivered it. `LogChannel`'s signals are named `LOG_E`, `LOG_W`,
`LOG_I`, and `LOG_D`, and UI objects connect their own `LOG_*` signals to
them signal-to-signal, so the logger sees the channel's signal and its name.
Renaming either side silently mislabels or drops lines;
`desktop_composition_test` pins the prefixes.

### Lines from destroyed dialogs are no longer dropped

A queued call reaches its receiver with a null `sender()` when the sender was
destroyed first, and `log_messages` drops such a line. Before 6j, the
stack-local DTC, BIU, and DataTerminal dialogs and short-lived flash classes
connected straight to the logger, so what they logged just before closing
could be lost. Every UI line now reaches the logger from `LogChannel`, which
outlives them; `relayedLineSurvivesItsSenderButADirectOneDoesNot` pins both
the fix and the old behavior.

### The remote wait is a direct-connected signal

`RemotePeer::wait_for_source` emits `wait_requested`, which the composition
connects to `RemoteUtility::waitForSource` with `Qt::DirectConnection`, so
the call blocks as the direct call did. A headless test cannot run the wait —
it loops until a peer answers — so `desktop_composition_test` checks only
that the signal is connected.

## Configuration session

`fastecu::config::ConfigSession` owns configuration state. `DesktopComposition` owns it and initializes it before building any other service. The same session reaches `MainWindow` through `MainWindowServices`.

- **One saved selection.** `AppConfig::selected_protocol_id` is the only selection state. Make, model, MCU, checksum, capabilities, and description are derived from the selected `ResolvedCarModel` and never cached. Vehicle rows keep file order, and a row's id is its position. Choosers sort only their presentation.
- **Paths.** Provisioned paths are fixed for the run. Effective paths take only the calibration and datalog directories from settings, so editing them never moves the config, kernel, definition, or syslog files.
- **Startup rejection (intentional behavior change).** `LegacyConfigAdapter` ignored provisioning and load failures, and startup proceeded into empty vehicle lists. A provisioning failure, an unreadable or malformed `fastecu.cfg`, an unreadable `protocols.cfg`, or one with no car models now shows the failing path and reason, then exits before `MainWindow` or the syslog thread exists, with no ECU I/O. A failed rewrite of a successfully loaded `fastecu.cfg` stays nonfatal and is shown as a startup warning. `DesktopCompositionTest::failedStartupBuildsNoServicesAndPerformsNoEcuIo` pins this and was mutation-checked.
- **Kept quirks.** An invalid saved row selects row 0 without replacing the saved transports or log protocol. Protocol-name selection takes the last matching row. An unresolved protocol reference stays `std::nullopt` and shows the single-space placeholder, with no capabilities. The writer's `logfiles_directory` versus the reader's `datalog_files_directory` still keeps the datalog directory from round-tripping, and `ConfigSessionSave.DatalogDirectoryDoesNotRoundTrip` pins it.

Bundled-default provisioning formerly copied CWD-relative config resources through the filesystem. It now reads `IResourceBundle` bytes and writes them through `IFileRepository`, so fresh config roots provision correctly before startup rejection is evaluated. Existing user files are preserved.

## Testing

### QtTest suites using Google Mock must fail on its failures

QtTest's exit status ignores Google Mock. A suite that uses it calls
`::testing::InitGoogleMock(&argc, argv)` in `main` and returns non-zero when
`::testing::Test::HasFailure()`. `test_mainwindow` lacked this until step
6d and passed with 14 violated expectations, all of them expectations that
had never matched the real call sequence.

### Grep does not prove code is dead

Two migrations found liveness that a grep got wrong in both directions. A WRX02
address-wraparound branch was guarded on a string that an earlier alias step
had already overwritten, so it never fired, yet grep found it "called" and it
was ported. Conversely, grep found a definition of `save_logger_conf` that did
not exist, because the whole function sat inside a `/* */` block. When retiring or porting code reached through
string comparisons or dispatch tables, confirm reachability against a built
binary or a comment-stripped parse.

### Pin every correction with a mutation check

When porting a correction (bounds check, reply gate, cancellation fix) with no
hardware to qualify it on, revert the production branch to the legacy
behavior, confirm one named test fails, then restore the tree. This catches
corrections that look tested but are unreachable or vacuously passing.

### `FakeCancellationToken` counts every check

`cancel_on_check(n)` counts every `cancelled()` call, including those inside
`FakeClock::sleep()` and `ScriptedKlineFlashTransport::read()`, not only the
executor's own checks. Derive checkpoint numbers by tracing every helper call;
when a number is off, fix the test, not the executor's check order.

### Resolving citations to deleted files

Executor comments and matrix notes cite `legacy … line N` against the deleted
per-family operation files, as they stood when each family migrated; some
comments also cite a wave's design doc. To read one, find the deleting commit
with `git log --diff-filter=D -- <path>` and view the file at its parent
(`git show <commit>^:<path>`). Wave 5 citations are pinned to `59f4e442`.

### C++ test-writing pitfalls

- `std::tuple{"a", "b", 0x20000U}` deduces `const char*` for string literals.
  Spell the element types when a caller needs `std::string_view`.
- Designated initializers must name fields in declaration order; skipping one
  to reach a later field is rejected, so `FlashAttemptResult{...}` spells every
  preceding field.
- `QCOMPARE` does not compile for types with no `QTest::toString`, such as
  `MemoryRegion` or `std::optional<bytes::Bytes>`. Use `QVERIFY(a == b)` rather
  than adding a printer only for the comparison.

## Family notes

### Hitachi M32R JTAG: removed, not migrated

`FlashEcuSubaruHitachiM32rJtag` was deleted in wave 6c-2. No `protocols.cfg`
entry could select it, its `read_mem()`/`write_mem()` were empty stubs that
reported success, and its probe ignored every failure. Porting the stubs would
have legitimized a no-op; porting only the probe would have added unreachable
code. If M32R JTAG is ever revived, design it as a new family: the removed
code's reply gates were unreliable and out of bounds on short replies. The
legacy source, including the probe's wire sequence and register map, is
recoverable at commit `766475b8` (`git show 766475b8:<path>`).

### Unisia Jecs M32R: two details left unresolved

Beyond the questions in the bench checklists, two pieces of legacy knowledge
are deliberately not implemented:

- Bootmode: status `0x5A` appears in the legacy list of block-write failure
  codes with no meaning; the program executor's failure details report it as
  "unknown".
- Both variants: legacy defines a 4800-baud switch (`B8 00 00 00 15`) that
  nothing calls. It was not ported.
