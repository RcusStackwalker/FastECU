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
   prompt).
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
the operator with a dedicated confirmation prompt.

### No shared helper for the Hitachi and Mitsubishi M32R K-Line families

The two families share SSM byte framing, but a common helper was rejected.
Mitsubishi requires its initial handshake with one timeout policy; Hitachi uses
optional probes, a short recovery-wake timeout, fallback parsing and tolerant
write acknowledgements. A helper would have to expose timeout and tolerance
policy at its boundary, recreating the configurable state machine that
[port-then-factor](protocol-generalization-opportunities.md) exists to avoid.

### Where port-then-factor shared code, and where it did not

- Wave 3 produced `subaru_tcu_cvt_mitsu_can_common` for the MH8111 and MH8104
  TCU families.
- Wave 4 found none of its cluster's protocol functions identical across all
  four families and shared only crypto key tables
  (`denso_iso15765_can_common.h`); wave 5's ISO-15765 families reuse them for
  stock security and kernel upload only.
- Wave 6b-2 shared only byte-identical seed and encrypt tables with the EEPROM
  K-Line executor (`denso_sh705x_kline_common.h`).
- Waves 1 and 6 declined any common code.
- `single_window_plan` covers declarative plan validation for ten kernel-free
  families; it was deliberately not widened to wave 5's kernel-backed,
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

### Fan logger definitions out once, at load

The legacy logging value structure's `enabled` fields start from the XML
attribute, then get overwritten at runtime by the ECU's capability response.
Re-fanning a parsed definition into that structure after logging has started
silently re-enables channels the ECU said it does not support. The fan-out runs
exactly once, at load, and no selection path may rewrite definition fields.
Fields are owned by the definition, by the operator's selection, by live
samples, or by both definition and runtime capability; only the definition
fan-out writes the first kind. The type system does not enforce this;
`selection_round_trip_leaves_enabled_flags_untouched` in
`legacy_logger_adapter_test.cpp` pins it.

### pugixml indents with a tab by default

The `QDomDocument::save(output, 4)` writer that pugixml replaced used four
spaces. Any pugixml writer for a file users already have on disk must set the
indent explicitly (as `logger_conf.cpp` does), or every existing file
reformats wholesale on the next write.

## Definitions and `FileActions`

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

## Build and guards

### Keep portable targets out of packages with a legacy glob

A `glob()`-based target silently absorbs any new source dropped into its
package, and a new portable target's glob can equally pull in a legacy Qt
file. Land portable code in its own package, and use explicit `srcs` lists for
any target sharing a package with legacy code.

### `PORTABLE_PACKAGES` lists targets, not packages

`bazel/portable_targets.bzl` maps each package to a list of target names. A
package already being listed does not cover a new target added to it: every new
portable `cc_library` must be added by name, or `//:portable_closure` never
checks it, without any failure or warning. This was missed once (wave 6a-1) and
left targets unguarded until noticed later.

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

`apps/desktop` must not join `serial_qt_compat`'s frozen visibility list. The
factory exposes construction only, returns an owner with a custom deleter, and
keeps `SerialPortActions` an incomplete type in `apps/desktop`. It connects
the facade's `LOG_*` signals to the sink with string-based `SIGNAL`/`SLOT`,
which fail only at runtime; `desktop_serial_factory_test` is what catches a
broken one.

### `Settings` saves through the shared `FileActions`

`Settings` used to build a throwaway `FileActions` reporting to a
`NullEventSink`. It now takes the caller's instance, so diagnostics from a
save reach the log window instead of being dropped.

## Logging composition

Step 6f moves the MUT/DMA, CDBG, and SSM registrations into
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

### `SerialDiagnosticLink` lives beside `service_functions`, not inside `serial_qt_compat`

The new `//src/platform/desktop/common/diagnostics` package reaches the
facade through `//src/platform/desktop/common/serial:serial_platform_api`,
the same same-layer handle that `service_functions` already uses, instead of
becoming a fourth caller added to the frozen `serial_qt_compat` visibility
list. `serial_diagnostic_link.h` forward-declares `SerialPortActions`, so
including it does not carry `serial_port_actions.h` to the UI; `MainWindow`
constructs a `SerialDiagnosticLink` from the facade pointer it already holds
and hands the dialogs an `IDiagnosticLink&`. This is also where `DtcWorker`
lives: it is a Qt adapter with no `SerialPortActions` include of its own, so
it needs no allowlist entry either.

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

- **The OpenPort five-baud response check compares ASCII, not values.**
  `five_baud_header`'s J2534 branch compares response bytes to the ASCII
  characters `'8'` and `'f'` at fixed offsets, while the direct-serial branch
  compares the same positions to the numeric bytes `0x08`/`0x08` and `0x8f`.
  Whether the OpenPort firmware genuinely echoes ASCII digits here, or the
  original code meant the numeric comparison and got it wrong, is not
  something to guess at from the source.
- **K-Line unframing keeps its length heuristics.** `unframe_data_response`
  and `unframe_dtc_list_response` strip a fixed number of leading bytes
  chosen by the frame's total length (`< 7`, `< 10`, otherwise) rather than
  by parsing a length field. It reproduces today's behavior exactly; a
  proper length-field parse is a separate, riskier change.
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

1. **DTC runs off the UI thread.** `DtcWorker` runs `run_dtc_session` on its
   own thread, so the dialog stays responsive during a run. Cancellation is
   best-effort, not step-by-step: `DtcRun` checks it only inside
   `IClock::sleep` and inside `IDiagnosticLink::read`/`read_obd`, exactly as
   `DesktopKlineFlashTransport` does elsewhere, not between every step. An
   already-cancelled run still costs one `open()` and one init exchange
   before the first sleep stops it; closing the dialog cancels the run and
   waits for the worker to stop, it does not cut it off instantly.
2. **DTC short responses fail cleanly.** An init or request response too
   short for the legacy unchecked `at(i)` becomes an init failure or
   `BadResponse` through `obd_frames.h`'s bounds-checked helpers, not an
   assert or undefined behavior.
3. **DataTerminal resets and sets every flag before opening.** `open()`
   applies every field of `KlineLinkConfig`/`CanLinkConfig` in one canonical
   order after a `reset()`, so DataTerminal no longer inherits header, CAN,
   or 29-bit flags an earlier tool left set. Each send already ended with a
   reset, so this only changes behavior when another tool left flags set
   right before DataTerminal's own `open()`.
4. **A failed DTC run always logs one error line.** `DtcOperations::finish`
   logs `"DTC operation failed: " + result.error_detail` for any failed run.
   Today some failures (an empty stored-DTC list, an ignored `open()` failure
   that let fast init proceed and fail later) ended silently. The `open()`
   case needs its own note: on iso14230, a failed `open()` during fast init
   does not end the run immediately. `fast_init()`'s `open()` failure is
   treated the same as any other fast-init failure, so `DtcRun::init()`
   still falls back to five-baud -- reproducing today's behavior, where the
   ignored `open_serial_port()` result let fast init's wire calls proceed
   and fail on their own. The run ends in `Disconnected` only if the
   five-baud path's own re-open also fails.
5. **DTC PID pages `0x81`-`0xE0` are accepted.** The legacy `request_data`
   compares a `QByteArray::at()` byte (a signed `char` on x86 and macOS)
   against the `uint8_t` PID, so the echo check for PID requests `0x80`,
   `0xA0`, and `0xC0` never matched and those supported-PID pages were
   logged as a wrong response and discarded. `check_response` compares
   `bytes::Byte` (unsigned), matching the behavior these platforms would
   already see if `char` were unsigned there (as it is on Linux/ARM).
6. **Each DTC run starts clean.** The legacy dialog's `fast_init_ok` member
   was never reset, so once one fast init passed in a dialog's lifetime,
   every later fast init in that same dialog passed its check too. Each
   `DtcRun` is a fresh object with no equivalent state, so every run's fast
   init is checked on its own merits.

Also a wording/format detail, not a behavior change: the legacy two-part
`LOG_I` for `"Supported PIDs: "` put a timestamp on the first part and no
linefeed before the second. `vehicle_info` now logs it as one `IEventSink::log`
call. It renders identically; nothing downstream parses the split.

## Flash-operation dispatch

### `start_ecu_operations` cleanup is a scope guard

The idle-line serial reset, battery-polling stop, and log-transport
re-selection run from a `qScopeGuard` constructed right after the port check.
Every exit from that point runs them. Before step 6d the Denso TCU service
path reached them through a `goto`, and the write-preflight early returns
("No file selected!", Cancel on the checksum warning) skipped them, leaving
battery polling running.

### The read slot is allocated early and released on failure

The read path still allocates `ecuCalDef[ecuCalDefIndex]` before dispatch,
because `update_protocol_info` reads it. Anything but a successful read with
data (cancel, failure, `Unsupported`, a handled TCU service action) deletes
it and resets the slot to `nullptr`, its state before the first read.

### `reset_serial_to_idle` lives beside the facade

It calls `SerialPortActions` methods, so it needs the facade's definition,
and `serial_qt_compat`'s frozen visibility list could not gain the UI
package that calls it. It lives in the serial package as `serial_idle`,
visible only to `//src/ui/desktop`. `FlashOperationController` only passes
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
declared once in `serial_port_actions_direct.h` and defined in exactly one
of `serial_port_actions_direct_unix.cpp` / `_windows.cpp`. Hook bodies were
moved verbatim, so a Windows regression shows up as a compile failure in
the wrong file rather than as changed behavior. Both J2534 packages publish
their API at `src/platform/desktop/j2534/j2534_api.h`, so the common code
has no guarded include.

### The binary names the platform

`desktop_serial_factory` links only the declaration of
`make_direct_serial_backend()`. `fastecu` and `fastecu-bench` each carry a
`select()` alias picking `direct_serial_backend_unix` or `_windows`; tests
use `direct_serial_backend_for_tests`. A target that forgets the
implementation fails to link.

### `STATUS_*` stay macros

`serial_facade_codes.h` keeps `STATUS_SUCCESS`/`STATUS_ERROR` and
`SERIAL_P*` as macros: the Windows SDK's `ntstatus.h` defines
`STATUS_SUCCESS` as a macro, which would break a constexpr of that name.

### One header cannot be moc'd by two targets

`qt_cc_library`'s moc genrule names its generated file after the header's
basename alone (`third_party/qt/qt.bzl`), not after the calling target, so
`direct_serial_backend_unix` and `_windows` cannot both list
`serial_port_actions_direct.h` in `hdrs` directly — the genrules would
collide at load time, on every platform, regardless of
`target_compatible_with`. The package-private
`serial_port_actions_direct_moc` target mocs that header once; both
per-OS targets depend on it instead of moc'ing the header themselves, and
`direct_serial_backend_for_tests` is an alias to whichever one matches the
host OS. This is a Bazel/Qt integration limit, not a design preference — a
future header split into per-OS pieces should keep it in mind.

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
code's reply gates were unreliable. The legacy source is recoverable at commit
`766475b8`. Its wire sequence, preserved here because no other copy remains:

- **Session:** ISO-14230 header off; ISO-14230, CAN, ISO-15765 and 29-bit all
  off; 4800 baud; no explicit parity.
- **Framing:** each request is `BE EF 00 <len> <payload…> <sum8>` (8-bit sum of
  every preceding byte), sent without echo drain, followed by a 10 ms delay and
  one 200 ms framed read.
- **Reply gate:** longer than 4 bytes, `[0]=BE`, `[1]=EF`, `[4]=<command>+0x40`,
  `[8]=0x31` (`SUB_KERNEL_JTAG_IR_ACK`). The gate read out of bounds on a 5–8
  byte reply; IDCODE and USERCODE also took `mid(9, 4)` and read `.at(0..3)`,
  out of bounds below 13 bytes.
- **Sequence** (payloads after the first two prefixed `0x40`,
  `SUB_KERNEL_JTAG_COMMAND`):
  1. Hard reset `80`, reply ignored.
  2. IDCODE `01`, expect `[4]=41`; `[9..12]` big-endian: version bits 31–28,
     part number 27–12, manufacturer 11–1.
  3. USERCODE `30`, expect `[4]=70`; `[9..12]`: ROM bits 11–8, ISA 7–4, SDI
     3–0.
  4. BSR sample `40 01 02 20 00 00 01 D7` (IR `SAMPLE`, sub-command
     `READ_BSR`, 471-bit scan), expect `[4]=80`, read until an empty reply.
  5. MON_CODE ×12: `40 10 01 20 <word 0/1/2>`, outer loop 4 × inner loop 3,
     expect `[4]=80`.
  6. MON_CODE word 3: `40 10 01 20 7F F4 F0 00`.
  7. MON_ACCESS `40 13 01 04 00 00 00 01`, then `…00`, expect `[4]=80`.
  8. MON_DATA ×5: `40 11 00 20`, expect `[4]=80`, 100 ms after each.
- **Tool-ROM code** (`inst_tool_rom_code`), four big-endian words:
  `D1 C0 FF 00 / A0 C1 3F FC / 20 44 F0 00 / 7F F4 F0 00`.
- **Register map** (from the removed `kernelcomms.h` block): commands
  `READ_USERCODE=0x30`, `JTAG_COMMAND=0x40`; IR `SAMPLE=0x01` (used),
  `EXTEST=0x00`, `IDCODE=0x02`, `BYPASS=0x3F`; registers `MON_CODE=0x10`,
  `MON_DATA=0x11`, `MON_ACCESS=0x13` (used), plus unused `IDCODE=0x02`,
  `USERCODE=0x03`, `MDM_SYSTEM=0x08`, `MDM_CONTROL=0x09`, `MDM_SETUP=0x0A`,
  `MTM_CONTROL=0x0F`, `MON_PARAM=0x12`, `DMA_RADDR=0x18`, `DMA_RDATA=0x19`,
  `DMA_RTYPE=0x1A`, `DMA_ACCESS=0x1B`, `RTDENB=0x20`; sub-commands `READ=0x00`,
  `WRITE=0x01`, `READ_BSR=0x02`; IR ack `0x31`.
- **Failure handling:** a failed IDCODE or USERCODE gate returned an error the
  caller ignored; a failed tool-ROM gate abandoned the rest of that sequence,
  also ignored; the operation then ran the empty stubs and reported success.
- **Dead code, never reached** (its one caller was commented out):
  `write_jtag_ir()` (`4b051f` reset, `4b0303` Shift-IR, `3b04<code>`,
  `6b0001`/`7b0001` last bit, `4b0101` Run/Idle, trailing `0D`);
  `write_jtag_dr()` (`4b0700` idle-8, `4b0201` Shift-DR, `3b1e<data>`,
  last-bit variants, `4b0101`, `0D`); `read_jtag_dr()` (`4b0700`, `4b0201`,
  `6b1f80000000` read-32, `4b0101`, `0D`); `read_response()` (reads until
  empty, keeps the last non-empty reply, takes `mid(4, [byte 3])` and reverses
  it). The end bit came from the first hex digit of the code or data string, tested
  against `0x2` in `write_jtag_ir()` and `0x8` in `write_jtag_dr()`.
  `set_rtdenb()` would have written IR `"20"`, then DR `"00000001"`, then read
  the DR back.

### Unisia Jecs M32R: two details left unresolved

Beyond the questions in the bench checklists, two pieces of legacy knowledge
are deliberately not implemented:

- Bootmode: status `0x5A` appears in the legacy list of block-write failure
  codes with no meaning; the program executor's failure details report it as
  "unknown".
- Both variants: legacy defines a 4800-baud switch (`B8 00 00 00 15`) that
  nothing calls. It was not ported.
