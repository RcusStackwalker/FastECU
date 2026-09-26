# Step 6g: move the diagnostic tools off the serial facade

## Goal

Advance the step 6 bullet "Remove compatibility wrappers, obsolete facades"
of the [modularization plan](../../modularization-plan.md) by draining the
`serial_qt_compat` allowlist's UI side. Step 6 has two UI entries left,
`//src/ui/desktop/biu` and `//src/ui/desktop`. This step handles the
self-contained diagnostic tools; step 6h (`MainWindow` connection service
and SSM ECU identification) removes the second entry.

Success means:

- The BIU, DTC, and DataTerminal dialogs no longer include
  `serial_port_actions.h`. They talk to a backend-owned port,
  `IDiagnosticLink`, through a platform adapter.
- `//src/ui/desktop/biu` is gone from the `serial_qt_compat` visibility list
  and from `FROZEN` in `scripts/check-serial-compat-allowlist.py`.
- The GRANDFATHERED `//src/ui/desktop` entry in
  `//src/platform/desktop/common/transport`'s `default_visibility` is gone:
  `mainwindow.h` no longer includes `fastecu_kline_transport.h`, and the UI
  reaches only the one adapter target that grants it by name.
- DTC protocol logic runs as a portable `DtcSession` in a new
  `//src/backend/diagnostics` package, registered in `PORTABLE_PACKAGES`,
  with scripted-link tests.
- The standalone MUT/DMA memory helpers are a portable, tested backend
  target instead of uncalled `MainWindow` members.
- Dead code is deleted: `hexcommander.{h,cpp,ui}`. (`kline_listener` and
  `canbus_listener` were deleted separately by #379.)

## Non-goals

- `MainWindow` connection orchestration: port listing, `open_serial_port`,
  `log_transport_changed`, vbatt, connect/disconnect, and the facade setup at
  the top of each `show_*_window`. That is step 6h.
- SSM ECU identification (`ecu_init`, `ssm_init`, `ssm_kline_init`,
  `ssm_can_init`). Step 6h.
- The developer toggles `can_listener`, `simulate_obd`, and
  `test_haltech_ic7_display`. The shipped `menu.cfg` does not expose them,
  but a user-edited menu can. Step 6h decides their fate.
- BIU command framing and DataTerminal's SSM header and CAN-ID prefix. Both
  dialogs are interactive byte pumps; their logic stays in the UI.
- Fixing the OpenPort five-baud ASCII comparison or DataTerminal's
  `delay(...)` parser (see [Behavior changes](#behavior-changes)); both are
  pinned and recorded, not fixed.
- Any change to `ErrorKind`.

## Current state (2026-09-26, `master` at `962d3e59`)

`serial_qt_compat`'s visibility list holds five entries:
`//src/platform/desktop/common/serial`, `//src/platform/desktop/common/transport`,
`//src/ui/desktop`, `//src/ui/desktop/biu`, and `//tests`.

UI files that include `serial_port_actions.h`, with their direct facade call
counts:

| File | Calls | Notes |
|---|---:|---|
| `dtc_operations.cpp` | 72 | OBD-II DTC read/clear; in scope |
| `mainwindow.cpp` | 53 | step 6h |
| `log_operations_ssm.cpp` | 36 | SSM identification (6h); MUT memory helpers (in scope) |
| `menu_actions.cpp` | 32 | `show_*_window` setup, dev toggles; step 6h |
| `hexcommander.cpp` | 26 | dead: not in any BUILD target |
| `dataterminal.cpp` | 26 | raw K-Line/CAN terminal; in scope |
| `biu/biu_operations_subaru.cpp` | 3 | `fast_init`, echo-checked write, read; in scope |

Other UI packages (`flash/operation`, `service_functions`) hold only a
forward-declared `SerialPortActions*` and pass it to platform code. That is
the pattern this step extends: the UI names the type but never includes its
header, and a platform adapter does the work.

### DTC today

`DtcOperations` is a modal `QDialog` that runs everything on the UI thread,
with a `processEvents` busy-wait `delay()`. Per protocol:

- **iso9141:** start byte `0x68`, tester `0xF1`, target `0x6A`; five-baud
  init at address `0x33`.
- **iso14230:** start byte `0xC0`, tester `0xF1`, target `0x33`; fast init
  with `0x81`. If fast init *fails*, it falls back to five-baud init.
- **iso15765:** 500 kbit/s, 11-bit, source `0x7E0`, destination `0x7E8`;
  sends `00 00 07 E0 01 00` and expects `0x41` at index 4.
- The SSM entries are present but disabled.

Five-baud init resets, clears all header flags, opens at 10400, sets P1 max
to 35 ms, sends the address byte, and checks the response. This depends on
`get_use_openport2_adapter()`:

- **J2534 (OpenPort):** P1 is set via `set_j2534_ioctl(kJ2534IoctlP1Max, 35)`.
  Success for iso9141 is `received[5] == '8' && received[7] == '8'`; for
  iso14230, `received[8] == '8' && received[9] == 'f'`. These are ASCII
  comparisons.
- **Direct serial:** P1 is set via `set_kline_timings(SERIAL_P1_MAX, 35)`.
  Success for iso9141 is `received[1] == 0x08 && received[2] == 0x08`; for
  iso14230, `received[2] == 0x8f`. P1 is then set back to 25 ms.
- On success, the ISO-9141 or ISO-14230 header flag is set and vehicle info
  is requested.

None of these indexings is bounds-checked.

Fast init resets, sets ISO-14230 on with the ISO-14230 header, opens at
10400, calls `fast_init(81)`, reads with `read_serial_data` (J2534) or
`read_serial_obd_data` (direct), and expects `83 F1 10 C1 E9 8F`.

After init, `request_vehicle_info` waits 500 ms, then makes these requests:

1. Mode `01` PIDs `00, 20, … C0`: the seven supported-PID bitmaps.
2. Mode `01` PID `01`: monitor status since DTCs cleared.
3. Mode `09` PIDs `01`–`06`: VIN length, VIN, CAL ID length, CAL ID, CVN
   length, CVN.

Every request is followed by a 250 ms delay.

Each request writes (with a 4-byte CAN-ID prefix on iso15765), then reads
frames until an empty read. The response byte sits at index 3 (K-Line) or
4 (iso15765):

- `0x7F` is logged as an NRC.
- A mismatched response ID is logged as wrong.

K-Line frames are unframed with length heuristics. `request_data` drops the
checksum, then strips `len−1`, 5, or 6 leading bytes for lengths `<7`,
`<10`, and otherwise. `request_dtc_list` strips `len−1` or 4. iso15765
frames strip `index+3` (data) or `index+2` (DTC list).

Read DTCs requests mode `03` (stored) and then `07` (pending); an empty
response for either is a failure. Each list is decoded as big-endian
16-bit codes, zeros are dropped, the codes are sorted as hex strings, and
each is logged with `dtc_description`. Clear DTCs first reads DTCs, then
sends mode `04` and expects `0x44`.

`closeEvent` calls `reset_connection()` and sets `kill_process`, which
nothing reads. `DtcOperations::run()` is uncalled.

### BIU today

`MainWindow::show_subaru_biu_window` does the following, then runs the
dialog modally and afterwards clears the ISO-14230 header flag:

1. resets the facade;
2. clears the ISO-14230 header flag and sets the ISO-14230 connection flag;
3. calls `MainWindow::open_serial_port()`, which also persists the chosen
   port to config and updates the status bar;
4. calls `change_port_speed("10400")`.

`BiuOperationsSubaru::send_biu_msg` calls `fast_init(output)` when
connecting and `write_serial_data_echo_check(output)` otherwise, then
`read_serial_data(800)`. A keep-alive `QTimer` drives periodic messages on
the UI thread.

### DataTerminal today

The K-Line send path does the following for each script line, and resets
the facade after the last line:

1. sets the ISO-14230 connection flag from the protocol box (SSM means off);
2. sets the baud rate (300–2,000,000), tester and target IDs, start byte
   `0x80`, CAN off, ISO-15765 off, and 11-bit IDs;
3. opens the port;
4. for each script line: adds the SSM header in the dialog when SSM is
   selected, writes with an echo check, waits 10 ms, and reads with
   `read_serial_data(200)`.

It does **not** reset before opening, and does not set any
`set_add_*_header` flag, so it inherits whatever an earlier tool left.

The CAN path does the same with CAN or ISO-15765, the bitrate, an 11- or
29-bit ID, and source/destination addresses. Each line is prefixed with the
4-byte tester ID.

`delay(N)` script lines are parsed with
`split(")").at(1).split("(").at(0)`. For `delay(100)` that yields an empty
string, so the delay is 0 ms on the K-Line path and the response delay is
0 ms on the CAN path.

### MUT memory helpers today

`MainWindow::mut_write_memory(addr, bytes)` refuses addresses outside
`0x4000–0xBFFF`. It then builds a `FastEcuKlineTransport`,
`AlreadyInMode(125000)`, and a `MutDmaDriver`, and calls `writeMemory`.
`mut_read_memory(addr, len)` reads in chunks of up to 40 bytes via
`planReadChannels`, `startFreeFormLog(ch, 0xA0, 0xA1)`, `pollOnce(50ms)`,
and `reassembleRead`, and stops at the first failure. Nothing calls either
function. They are the only reason `mainwindow.h` includes
`fastecu_kline_transport.h`.

### Facade setter semantics

Every setter this step uses only records state, both in the facade
(`runOnBackend` forwarding) and in the direct backend
(`serial_port_actions_direct.h`). Only `open_serial_port`,
`change_port_speed`, `reset_connection`, the init calls, and reads and
writes touch the wire. Setter *order* is therefore not observable, and the
adapter may apply one canonical order.

## Design

### The port: `IDiagnosticLink`

Header: `src/backend/protocol/idiagnostic_link.h`. It sits beside
`IKlineTransport`, `ICanTransport`, and `ISsmTransport`, per the rule that
transport ports stay in `src/backend/protocol`. It is Qt-free and
byte-native, and is added to the existing portable `//src/backend/protocol`
package.

```cpp
namespace fastecu::diagnostics
{
enum class KlineHeader { None, Ssm, Iso9141, Iso14230 };

struct KlineLinkConfig
{
    KlineHeader header;
    bool iso14230_connection;
    int baud;
    std::uint8_t start_byte;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};

struct CanLinkConfig
{
    bool iso15765;          // false: raw CAN
    int bitrate;
    bool extended_id;       // 29-bit
    std::uint32_t source_id;
    std::uint32_t destination_id;
};

class IDiagnosticLink
{
  public:
    using OptionalBytes = std::optional<bytes::Bytes>;
    virtual ~IDiagnosticLink() = default;

    // reset, apply every field of the config, open. Disconnected if the
    // adapter reports no opened port.
    virtual Status open(const KlineLinkConfig&) = 0;
    virtual Status open(const CanLinkConfig&) = 0;
    virtual Status reset() = 0;

    virtual Status set_header(KlineHeader) = 0;             // mid-session
    virtual Status set_p1_max(std::chrono::milliseconds) = 0;
    virtual Result<bytes::Bytes> five_baud_init(std::uint8_t address) = 0; // raw, uninterpreted
    virtual Status fast_init(bytes::ByteView wakeup) = 0;

    // Bytes exactly as the facade expects them (CAN writes carry the 4-byte
    // ID prefix); no reframing. Returns the facade's echo-checked result.
    virtual Result<bytes::Bytes> write(bytes::ByteView) = 0;
    // A deadline is a successful empty optional, as with ISsmTransport.
    virtual Result<OptionalBytes> read(std::chrono::milliseconds, const ICancellationToken&) = 0;     // read_serial_data
    virtual Result<OptionalBytes> read_obd(std::chrono::milliseconds, const ICancellationToken&) = 0; // read_serial_obd_data

    virtual bool uses_j2534() const = 0;
};
} // namespace fastecu::diagnostics
```

Design rules:

- **Byte-faithful.** The port is a thin port. It does not frame, unframe,
  or add headers or checksums. Those stay with the facade (when a header
  flag is set) or with the caller.
- **`open()` sets every field.** Each `open()` is: `reset_connection`, then
  every relevant setter in one canonical order, then `open_serial_port`.
  - **K-Line setters:** ISO-14230 connection; the SSM, ISO-9141, and
    ISO-14230 header flags derived from `header`; baud; start byte;
    tester ID; target ID; CAN off; ISO-15765 off; 11-bit IDs.
  - **CAN setters:** ISO-14230 connection off; all header flags off; CAN or
    ISO-15765 on (the other off); bitrate; 29-bit flag; source and
    destination addresses.
- **Adapter-dependent behavior is visible, not hidden.** The session branches
  on `uses_j2534()` for the five-baud response check and for `read` vs
  `read_obd`, as the dialog does today. Only the P1 mechanism is hidden:
  `set_p1_max` calls `set_j2534_ioctl(kJ2534IoctlP1Max, ms)` on J2534 and
  `set_kline_timings(SERIAL_P1_MAX, ms)` otherwise.
- **Errors.** No new `ErrorKind`. The adapter maps:
  - a facade `STATUS_ERROR`, a `false` setter, or an empty
    `open_serial_port` result → `Disconnected`;
  - cancellation observed before a call → `Cancelled`.

  Response interpretation belongs to the session, which uses `BadResponse`
  for NRCs, short frames, and mismatched response IDs.
- **Cancellation is best-effort,** exactly as in `DesktopKlineFlashTransport`.
  The token is checked before each call. An in-flight read returns through
  its own bounded timeout.

### Platform adapter: `SerialDiagnosticLink`

Target `//src/platform/desktop/common/transport:serial_diagnostic_link`
implements `IDiagnosticLink` over a non-owning `SerialPortActions*`. It lives
in the `transport` package because that package is already on the frozen
`serial_qt_compat` list and holds the other facade adapters. A new platform
package would need a new allowlist entry, which the ratchet forbids.

Visibility is per target. The package `default_visibility` loses its
GRANDFATHERED `//src/ui/desktop` entry. `:serial_diagnostic_link` alone
grants these packages:

- `//src/ui/desktop`, for DTC, DataTerminal, and `show_subaru_biu_window`;
- `//src/ui/desktop/biu`;
- `//src/platform/desktop/common/diagnostics`, for the DTC worker.

This is the `ui → platform` direction the layering permits, narrowed from a
whole package to one adapter, as the `service_functions` grant already is.

The UI constructs the adapter from the forward-declared `SerialPortActions*`
it already holds, as `ServiceFunctionDialog` does with
`SerialPortActionsConfigurator`. No composition-root change is needed.

A package-owned `FakeDiagnosticLink` (`testonly`) goes in
`//src/backend/protocol/testing`, beside the existing transport fakes.

### MUT memory: `//src/backend/protocol:mut_memory`

- `Status write_memory(IKlineTransport&, std::uint16_t addr, bytes::ByteView, const ICancellationToken&)`
  refuses `addr < 0x4000 || addr > 0xBFFF` with `InvalidConfig` before any
  I/O. The guard is unchanged; it must not be relaxed.
- `Result<bytes::Bytes> read_memory(IKlineTransport&, std::uint16_t addr, std::size_t len, const ICancellationToken&)`
  keeps the 40-byte chunking and the `0xA0`/`0xA1` request IDs. It returns
  what was read before the first failure (today's truncation semantics) and
  an error only if the first chunk fails.

Both build `AlreadyInMode(125000)` and a `MutDmaDriver` internally, as the
`MainWindow` versions do. Any existing `MutDmaDriver` failure keeps its
`ErrorKind`. The two `MainWindow` members and the `fastecu_kline_transport.h`
include are deleted. Wiring a UI or bench-CLI caller is out of scope.

### DTC: `//src/backend/diagnostics`

A new portable package, registered in `PORTABLE_PACKAGES`. It is distinct
from `//src/algorithms/diagnostics`, whose `dtc_description` and
`nrc_description` it uses.

Pure helpers (target `:obd_frames`):

- `unframe_data_response(protocol, frame)` and
  `unframe_dtc_list_response(protocol, frame)`, which reproduce today's
  K-Line length heuristics and iso15765 offsets exactly;
- `check_response(protocol, frame, mode, pid?)`, returning `Ok`, `Nrc`, or
  `WrongId` with the NRC description;
- `five_baud_accepted(protocol, response, uses_j2534)` and
  `fast_init_accepted(response)`, bounds-checked;
- `decode_supported_pids(page, bytes)` and `decode_dtcs(bytes)`, the latter
  dropping zeros and sorting as today.

Session (target `:dtc_session`):

```cpp
enum class ObdProtocol { Iso9141, Iso14230, Iso15765 };
enum class DtcOperation { Read, Clear };
struct DtcRequest { ObdProtocol protocol; DtcOperation operation; };

struct DtcReport
{
    std::vector<SupportedPidPage> supported_pids;
    std::optional<bytes::Bytes> monitor_status, vin_length, vin, cal_id_length, cal_id, cvn_length, cvn;
    std::vector<std::uint16_t> stored, pending;
    bool cleared = false;
};

Result<DtcReport> run_dtc_session(const DtcRequest&, IDiagnosticLink&, IClock&,
                                  const ICancellationToken&, IEventSink&);
```

The session reproduces the sequence in [DTC today](#dtc-today), including
every sleep (500 ms before vehicle info, 250 ms after each request, and
250 ms after each DTC list). Sleeps go through `IClock::sleep`, so
cancellation interrupts them. Log lines go through `IEventSink::log` with
today's wording and levels. At the end, regardless of outcome, the session
restores the link: it clears all header flags and calls `reset()`, which is
today's `select_operation` epilogue.

### DTC: `DtcWorker` and the dialog

`//src/platform/desktop/common/diagnostics:dtc_worker` is a `QThread` that
owns the `SerialDiagnosticLink`, a `QtClock`, and a
`ManualCancellationToken`, and runs `run_dtc_session` once. It mirrors
`ServiceFunctionWorker` without gates:

- `requestStop()` is safe from any thread;
- the destructor stops and joins;
- it emits a `logEvent(int, QString)` per log line and a `finished`
  signal carrying a Qt-friendly `DtcWorkerResult`;
- it adapts `IEventSink` to queued signals.

This package holds only Qt adapters and no `SerialPortActions` include, so
it is not on the allowlist.

`DtcOperations` keeps its `.ui` form and protocol box. Its behavior changes
as follows:

- The Read and Clear buttons start a worker, and are disabled while it runs.
- `closeEvent` calls `requestStop()` and waits for the worker, whose session
  performs the reset.
- Log lines forward to the existing `LOG_*` signals.
- The dialog drops `delay()`, `run()`, `kill_process`, the protocol
  constants, and its `SerialPortActions` include. It keeps the
  forward-declared pointer.

### BIU

`BiuOperationsSubaru` takes an `IDiagnosticLink&` instead of
`SerialPortActions*`. `send_biu_msg` maps directly:

- `fast_init(output)` → `link.fast_init`;
- `write_serial_data_echo_check` → `link.write`;
- `read_serial_data(800)` → `link.read(800ms, never_cancelled)`.

The keep-alive `QTimer` and the synchronous UI-thread model are unchanged.

`show_subaru_biu_window`'s preamble stays in `MainWindow` unchanged. It
calls `MainWindow::open_serial_port()`, which persists config and updates
the status bar, and that is step 6h's connection work. The window
constructs a `SerialDiagnosticLink` over the already-open facade and hands
it to the dialog without calling `open()`.

`//src/ui/desktop/biu` drops `serial_qt_compat` from its deps. The entry is
removed from the visibility list and from `FROZEN`.

### DataTerminal

`DataTerminal` constructs a `SerialDiagnosticLink` from its forward-declared
pointer. The K-Line path validates the form as today, then calls
`open(KlineLinkConfig{header = None, iso14230_connection = protocol == "iso14230", baud, 0x80, tester, target})`.
The CAN path calls
`open(CanLinkConfig{iso15765 = protocol == "iso15765", bitrate, extended_id = index == 1, tester, target})`.

As today, an invalid baud rate or protocol skips `open()` but still runs the
send loop against whatever state the link is in; that is preserved, not
fixed. Script sending, SSM header and CAN prefix assembly,
message boxes, and file reading stay in the dialog. Writes and reads go
through `link.write` and `link.read(200ms, never_cancelled)`, and each
send ends with `link.reset()`.

## PR sequence

A `gh stack`, as with step 6e:

1. **6g spec and plan** (docs only).
2. **6g-1: the port and cleanup.**
   - `IDiagnosticLink` and `FakeDiagnosticLink`;
   - `SerialDiagnosticLink` with setter-order and J2534/direct tests;
   - `//src/backend/protocol:mut_memory` with tests; the `MainWindow`
     helpers and the `fastecu_kline_transport.h` include deleted, the
     `//src/ui/desktop:transport` dependency dropped, and the GRANDFATHERED
     transport-visibility entry removed;
   - `hexcommander.{h,cpp,ui}` deleted, along with the BUILD comment that
     names them.
3. **6g-2: BIU** moves to the port, and `//src/ui/desktop/biu` leaves the
   allowlist.
4. **6g-3: DataTerminal** moves to the port.
5. **6g-4: DTC** gets the `//src/backend/diagnostics` package, `DtcWorker`,
   and the dialog rework. It also carries the close-out:
   - the modularization plan's step 6 entry;
   - a "Diagnostic tools" section in the design notes, recording the
     OpenPort five-baud ASCII check and the `delay(...)` parser;
   - tech-debt updates: the allowlist count, and entries for both pinned
     quirks;
   - the bench checklist;
   - deletion of this spec and its plan.

## Behavior changes

1. **DTC runs off the UI thread.** The dialog stays responsive. Close now
   cancels the session between steps and during sleeps, instead of only
   resetting the facade. Precedent: service functions.
2. **DTC short responses fail cleanly.** An init or request response too
   short for today's unchecked `at(i)` becomes an init failure or
   `BadResponse`, not an assert or undefined behavior.
3. **DataTerminal resets and sets every flag before opening.** It no longer
   inherits header, CAN, or 29-bit flags from an earlier tool. Each send
   already ends with a reset, so this changes behavior only when another
   tool left flags set.
4. **Removed:** `hexcommander.*` (never built), the uncalled MUT memory
   members (moved), and DTC's uncalled `run()` and unread `kill_process`.

**Deliberately unchanged, pinned by tests:**
- the OpenPort five-baud check compares response bytes to ASCII `'8'` and
  `'f'` at fixed offsets. That looks wrong, but changing it needs a bench
  capture;
- the K-Line unframing length heuristics;
- DataTerminal's `delay(...)` parse yielding 0;
- the ISO-14230 fallback from a *failed* fast init to five-baud.

## Risk and verification

- **Wire sequences.** The adapter is tested against the real facade over a
  Google Mock backend (`fake_backed_serial`). Each `open()` config asserts
  the full setter set and the reset-then-open framing, and `set_p1_max`
  asserts the J2534/direct split. `DtcSession` tests script a
  `FakeDiagnosticLink` for:
  - each protocol and adapter kind;
  - the fast-init fallback;
  - an NRC;
  - a wrong ID;
  - a short frame;
  - multi-frame responses;
  - cancellation during a sleep and between requests;
  - clear success and failure.

  Every sleep duration is asserted through a fake clock. Pure helpers get
  golden vectors derived from today's code, each pinned with a mutation
  check (design notes, "Pin every correction with a mutation check").
- **MUT memory.** Guard tests at `0x3FFF`, `0x4000`, `0xBFFF`, and `0xC000`
  (a refused write performs no I/O); chunking tests at 39, 40, 41, and 80
  bytes; and truncation after a failed chunk.
- **UI.** A DTC dialog test (`fastecu_qttest`, offscreen) checks that Close
  during a run stops and joins the worker, and that the buttons re-enable
  on completion. The BIU and DataTerminal changes are covered by the
  adapter tests plus the bench checklist; neither dialog has tests today.
- **Guards, every PR:**
  - `bazel test --config=release //...`;
  - `//:portable_closure`, with `src/backend/diagnostics` registered in 6g-4;
  - `//:serial_compat_allowlist`, which must shrink in 6g-2 and never grow;
  - `prek run --all-files`;
  - `bazel run //:clang_tidy_report_changed`.
- **Hardware.** A new `docs/diagnostics-bench-checklist.md` covers:
  - DTC read and clear over iso9141, iso14230 (both fast init and the
    five-baud fallback), and iso15765, each on OpenPort 2.0 and on a direct
    K-Line cable where applicable;
  - Close during a run;
  - a BIU connect and a keep-alive;
  - a DataTerminal K-Line SSM send and a CAN iso15765 send.

  Nothing is marked qualified from unit tests.
