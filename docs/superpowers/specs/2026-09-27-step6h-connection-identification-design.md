# Step 6h: move `MainWindow`'s connection handling and SSM identification off the serial facade

## Goal

Finish draining the UI side of the `serial_qt_compat` allowlist, continuing
the step 6 bullet "Remove compatibility wrappers, obsolete facades" in the
[modularization plan](../../modularization-plan.md). Step 6g removed
`//src/ui/desktop/biu`. This step removes the last UI entry,
`//src/ui/desktop:__pkg__`, which the [tech-debt roadmap](../../tech-debt.md)
assigns to 6h.

Success means:

- No file in the `//src/ui/desktop` package includes `serial_port_actions.h`.
  That covers `mainwindow.cpp`, `menu_actions.cpp`, `log_operations_ssm.cpp`,
  `mainwindow.h`, and `mainwindow_test.cpp`.
- `//src/ui/desktop:__pkg__` is removed from `serial_qt_compat`'s
  `visibility` list and from `FROZEN` in
  `scripts/check-serial-compat-allowlist.py`. What remains is the serial
  package itself, `//src/platform/desktop/common/transport`, and `//tests`.
- SSM ECU identification runs as a portable `identify_ssm_ecu` in
  `//src/backend/diagnostics`, with scripted-link tests, and the desktop runs
  it on a worker thread.
- `MainWindow`'s port listing, opening, transport setup, idle reset, and
  battery reading go through a platform adapter, `AdapterConnection`, not
  through direct facade calls.
- The developer toggles `can_listener`, `simulate_obd`, and
  `test_haltech_ic7_display` are deleted.

## Non-goals

- Switching CAN identification from UDS `22 F1 82` to SSM-over-CAN `AA`/`EA`,
  which would return capability bytes. That is a larger behavior change and
  becomes a tech-debt entry.
- Fixing the dead raw-`CAN` identification branch, or changing the CAN/iso15765
  identifier widths that `log_transport_changed` sets. Both are preserved.
- Making `MainWindow`'s `ecuid`, `ecu_init_complete`, or `LogValues` handling
  portable. These stay as presentation state.
- Deleting `serial_qt_compat`. Once this step lands, only same-layer entries
  and `//tests` are left, which makes it the obvious next candidate.
- Handing the flash controller, service-function dialog, diagnostic link, or
  `reset_serial_to_idle` anything other than the facade reference they take
  today. They forward-declare `SerialPortActions` and do not need its header.

## Current state (2026-09-27, `master` at `59b81a19`)

`MainWindow` holds `SerialPortActions *serial` and calls it directly in
`mainwindow.cpp`, `menu_actions.cpp`, and `log_operations_ssm.cpp`. The
calls fall into four clusters.

### Developer toggles

`can_listener` (`mainwindow.cpp`), and `simulate_obd` and
`test_haltech_ic7_display` (`menu_actions.cpp`), together make about 39 facade
calls. Each runs an unbounded `while (flag)` loop on the UI thread, kept alive
by `delay()`'s `processEvents`. They are reached through `toggle_*` handlers
that look up menu actions by the texts "CAN listener", "Simulate OBD", and
"Haltech IC-7", and through the `MenuCommand` values `CanListener`,
`SimulateObd`, and `HaltechIc7`. None of those actions has ever been in the
shipped `resources/shared/config/menu.cfg` (`git log -S` finds no history);
only a hand-edited menu reaches them.

### SSM identification

`ecu_init` (`log_operations_ssm.cpp`) does nothing unless the port is open
and the make is `"Subaru"`. It then dispatches on the log transport:

- **`SSM` → `ssm_init` (SSM1).** Reset; baud 1953, even parity; open. It
  echo-check-writes `78 12 34 00` and reads 10 × 500 ms. It *plain*-writes
  `00 46 48 49` and reads 2 × 500 ms. It plain-writes `12 00 00 00` and reads
  500 ms. If `len == [3] + 5`, it takes the ECU ID and log capabilities, then
  drains the line in 100 ms reads, re-parsing each frame. Two entries in
  `protocols.cfg` offer this transport.
- **`K-Line` → `ssm_kline_init` (SSM2).** It does not open the port; it uses
  the state that `connect_to_ecu` and `log_transport_changed` left (4800
  baud, no header flags). It echo-check-writes
  `add_ssm_header([BF])` = `80 {10|18} F0 01 BF cs`, with target `0x10` for
  ECU or `0x18` for TCU as chosen by the toolbar radio button. It then reads
  once with the short timeout, up to 10 × 50 ms for a 4-byte header, and up
  to 10 × 50 ms for `[3] + 5` bytes. The only check it applies is that
  length. An `ecu_init_started` flag guards against the re-entry that
  `delay()` makes possible.
- **`CAN` / `iso15765` → `ssm_can_init`.** The first branch tests
  `log_protocol == "CAN"`. No configuration sets that protocol, so the branch
  is dead. The iso15765 branch reopens as ISO-15765, 500 kbit/s, 11-bit, with
  source `0x7E0`/`0x7E1` and destination `0x7E8`. It sends
  `00 00 07 E0|E1 22 F1 82` and reads 100 ms. On `len > 7` and
  `[4..6] == 62 F1 82`, the ID is the hex of bytes 7 onward, and no
  capability parsing happens.

`parse_ecuid` drops 8 bytes (5 of header plus 3 unknown) and hex-encodes the
next 5. `parse_log_value_list` drops the 5-byte header and sets the
`LogValues` enabled flags from bit positions, then calls `read_logger_conf`
and `update_logboxes`.

### Connection lifecycle

- The constructor fills the port list from `check_serial_ports()` and calls
  `set_serial_port_baudrate` and `set_serial_port`. When there is a peer, it
  calls `waitForSource`, and it connects `stateChanged`.
- `check_serial_ports` resets and clears every flag, sets 4800, re-emits the
  log-transport change, and relists the ports.
- `open_serial_port` selects the chosen port, opens it, and updates the
  status bar and the saved `configValues->serial_port`.
- `log_transport_changed` sets the flags for CAN (raw, 11-bit, 500k),
  iso15765 (29-bit, 500k), or K-Line+SSM (`change_port_speed(4800)`). It then
  saves the config and resets.
- `connect_to_ecu` resets, opens, and disables the port list and refresh
  button. If the port opened, it runs `ecu_init` up to 5 times with
  `delay(500)`. If identification fails it calls `disconnect_from_ecu`, but it
  **returns `STATUS_SUCCESS` either way**; only a port that fails to open
  returns `STATUS_ERROR`. For a non-Subaru make, `ecu_init` is a no-op, so
  connecting costs 2.5 s and then disconnects.
- `disconnect_from_ecu` resets, sets 4800 and no parity, and re-enables the
  controls.
- `update_vbatt` (on a timer during flash operations) reads `read_vbatt()`
  only when `get_use_openport2_adapter()` is true.
- Logging start (`menu_actions.cpp`, around line 544) calls `connect_to_ecu`
  when `!ecu_init_complete`, and aborts only on `STATUS_ERROR`.

### Dialog launch preparation

- `show_dtc_window`: reset, then select the port. `run_dtc_session` opens its
  own link.
- `show_terminal_window`: select the port.
- `show_subaru_biu_window`: reset, clear the iso14230 header flag, set
  iso14230 connection, open, `change_port_speed(10400)`. After `exec()` it
  clears the iso14230 header flag. BIU relies on this pre-open.
- `start_ecu_operations`: selects the port, and its scope guard ends with
  `change_port_speed(4800)`.

### The reference implementation

RomRaider (`io/protocol/ssm/iso9141`, `io/serial/connection`) implements SSM2
only and has no SSM1. Its K-Line init is `80 10 F0 01 BF 40` at 4800 8N1. It
flushes stale bytes, writes, and treats the K-Line echo as part of the
response, stripping it by request length. It has no separate write that skips
the echo. It validates the init response: header `0x80`, tester `0xF0`,
target, the length byte, response code `0xFF`, and the checksum. Its ECU-ID
offset matches `parse_ecuid`. On CAN it uses `AA` → `EA` to 0x7E0.

## Design

### Dev toggles: deleted

Delete `can_listener`, `simulate_obd`, and `test_haltech_ic7_display`, along
with `toggle_can_listener`, `toggle_simulate_obd`, `toggle_haltech_ic7_display`,
the `can_listener_on`, `simulate_obd_on`, and `haltech_ic7_display_on` flags,
the three `MenuCommand` enumerators, and their string mappings. A `menu.cfg`
naming `can_listener`, `simulate_obd`, or `haltech_ic7` then parses as an
unknown command and takes the existing unknown-command path. A
`menu_command` test pins that. The design notes record the removal and why:
the actions were never shipped, and each one looped forever on the UI thread.

### Port change: parity in `KlineLinkConfig`

`src/backend/protocol/idiagnostic_link.h` gains:

```cpp
enum class Parity
{
    None,
    Even,
};
```

It also gains `Parity parity = Parity::None;` in `KlineLinkConfig`.
`SerialDiagnosticLink::open(const KlineLinkConfig&)` applies it through
`set_serial_port_parity` (`QSerialPort::NoParity` or `EvenParity`) as one more
named setter in its `run_setters` list, before `open_serial_port()`. The
package-owned fake link records it. `IDiagnosticLink` gains **no** write that
skips the echo check: SSM1's two plain writes become echo-checked (see
[Behavior changes](#behavior-changes)).

### Portable identification: `//src/backend/diagnostics:ssm_identify`

```cpp
namespace fastecu::diagnostics
{
enum class SsmVariant { Ssm1, KlineSsm2, Iso15765Uds };
enum class SsmTarget { Ecu, Tcu };

struct SsmIdentifyRequest
{
    SsmVariant variant;
    SsmTarget target = SsmTarget::Ecu;
};

struct SsmIdentity
{
    std::string ecu_id;         // uppercase hex, no separators
    bytes::Bytes init_response; // full frame; empty for Iso15765Uds
};

// One attempt. Retry policy belongs to the caller.
Result<SsmIdentity> identify_ssm_ecu(IDiagnosticLink& link, IClock& clock,
                                     const ICancellationToken& cancellation,
                                     const SsmIdentifyRequest& request);

// Exposed for tests and for callers that frame SSM2 themselves.
bytes::Bytes ssm_frame(bytes::ByteView payload, SsmTarget target);
std::optional<std::string> parse_ssm_ecu_id(bytes::ByteView init_response);
}
```

`ssm_frame` produces `80 {10|18} F0 len payload… cs`, where `cs` is the
8-bit sum. The legacy `add_ssm_header`'s `dec_0x100` parameter was always
`false`, so it is not carried over. `parse_ssm_ecu_id` returns the 5 bytes
at offset 8 as uppercase hex, or `nullopt` when the frame is shorter than 13
bytes; the legacy code silently produced a shorter ID in that case.

**`KlineSsm2`:**

1. `link.open(KlineLinkConfig{.header = None, .baud = 4800, .parity = None})`.
   This makes explicit the state `ssm_kline_init` inherited.
2. `link.write(ssm_frame({0xBF}, target))`, echo-checked.
3. Reads with the legacy budgets: a 200 ms clock sleep, then one read of
   200 ms (the legacy `serial_read_short_timeout`); up to 10 × 50 ms until
   4 bytes are present;
   up to 10 × 50 ms until `[3] + 5` bytes are present. It stops at the first
   budget exhausted and returns `Timeout` if nothing arrived and
   `BadResponse` if the frame is short.
4. Validation, following RomRaider: `[0] == 0x80`, `[1] == 0xF0`,
   `[2] ==` the request's target (`0x10` or `0x18`), `[3] == len − 5`,
   `[4] == 0xFF`, and the last byte equals the checksum of everything before
   it. The first failure returns `BadResponse` naming the field and carrying
   the frame in hex.
5. It returns `{parse_ssm_ecu_id(frame), frame}`. An ID that does not parse is
   `BadResponse`.

**`Ssm1`:**

1. `link.open(KlineLinkConfig{.header = None, .baud = 1953, .parity = Even})`.
2. It writes `78 12 34 00` and reads 10 × 500 ms, accumulating the bytes. It
   logs them and does not check them, as today.
3. It writes `00 46 48 49` and reads 2 × 500 ms, the same way.
4. It writes `12 00 00 00` and reads 500 ms.
5. On `len == [3] + 5` it succeeds. The legacy code then drains the line in
   100 ms reads, re-parsing every frame, so the last one wins. The port keeps
   reading 100 ms at a time until a read comes back empty, and the last
   non-empty read that satisfies the length check becomes `init_response`.
   RomRaider's checks are SSM2-specific and are not applied here. **Pinned:**
   the length is the only check on SSM1.
6. All three writes are echo-checked `link.write`.

**`Iso15765Uds`:**

1. `link.open(CanLinkConfig{.iso15765 = true, .bitrate = 500000, .extended_id = false, .source_id = 0x7E0 or 0x7E1, .destination_id = 0x7E8})`.
2. It writes `00 00 07 E0|E1 22 F1 82` and reads 100 ms.
3. On `len > 7` and `[4..6] == 62 F1 82`, the result is
   `{hex(bytes[7..]), {}}`. Anything else is `BadResponse`; no bytes at all is
   `Timeout`.

**Errors:** cancellation during a read or sleep → `Cancelled`. Link errors
pass through unchanged. No exceptions escape. The function logs nothing;
the worker turns results into log lines.

### Platform: `SsmIdentifyWorker`

`//src/platform/desktop/common/diagnostics:ssm_identify_worker` is a
`QThread` that follows `DtcWorker`. It takes the facade (forward-declared),
builds a `SerialDiagnosticLink`, a steady `IClock`, and a cancellation token,
and runs up to 5 attempts of `identify_ssm_ecu`, sleeping 500 ms through the
clock between them. `Cancelled` ends the loop immediately; `Timeout` and
`BadResponse` go to the next attempt, and the last error is reported if all
attempts fail. Each failed attempt is reported through a `progress(QString)`
signal. `completed(bool ok, QString ecu_id, QByteArray init_response,
QString error)` is emitted exactly once per `run()`. `cancel()` is safe from
any thread, and the destructor cancels and joins.

### Platform: `AdapterConnection`

The new package `//src/platform/desktop/common/connection` holds one
`qt_cc_library`, `adapter_connection`, reaching the facade through
`//src/platform/desktop/common/serial:serial_platform_api`. It needs no
`serial_qt_compat` entry. `adapter_connection.h` forward-declares
`SerialPortActions`. The package's visibility grants `//src/ui/desktop` and
`//apps/desktop` by name.

| Member | Legacy calls it replaces |
|---|---|
| `QStringList available_ports()` | `check_serial_ports()` |
| `void set_initial_port(const QString& port, const QString& baud)` | the constructor's `set_serial_port_baudrate` + `set_serial_port` |
| `void select_port(const QString& port)` | `set_serial_port_list({port})` |
| `QString open()` | `open_serial_port()`; empty means failure |
| `bool is_open() const` | `is_serial_port_open()` |
| `void reset()` | `reset_connection()` |
| `void apply_log_transport(LogTransport transport, bool ssm_protocol)` | `log_transport_changed`'s flag block, including the final `reset_connection()` |
| `void clear_link_flags()` | `check_serial_ports`'s flag reset: reset; iso14230, 29-bit, iso14230 header, CAN, iso15765 all false; 4800 |
| `void return_to_idle()` | `disconnect_from_ecu`'s reset: reset; 4800; no parity. It leaves the link flags alone |
| `void set_port_speed(int baud)` | `change_port_speed` |
| `std::optional<unsigned long> battery_millivolts()` | `get_use_openport2_adapter()` + `read_vbatt()` |
| `void wait_for_source()` | `waitForSource()` |
| `signal stateChanged(...)` | forwarded from the facade |
| `SerialPortActions& facade()` | the opaque handle for the four handoffs |

`LogTransport` is `{ Can, Iso15765, KLine, Ssm, Other }`, parsed from the
combo text in `MainWindow`. The call order inside each member reproduces the
legacy order exactly, and the tests pin it against the fake backend.
`clear_link_flags` and `return_to_idle` stay separate on purpose. The link
flags that `log_transport_changed` sets must survive a disconnect, because
`connect_to_ecu` does not reapply them. If a disconnect cleared them, the
next connect on a CAN transport would open as K-Line. Conversely,
`check_serial_ports` never touched parity, so an even parity left by SSM1
survives a port refresh. That quirk is pinned, not fixed.

### `MainWindow`

- `MainWindowServices::serial` becomes `AdapterConnection& connection`.
  `DesktopComposition` builds the adapter over its facade and owns it, and
  destroys it before the facade.
- `mainwindow.h` keeps no `SerialPortActions*` member. The four handoffs use
  `connection.facade()`.
- **Connect becomes asynchronous.** `connect_to_ecu(std::function<void(bool)> on_done)`:
  1. clears `ecuid` and `ecu_init_complete`, clears the status bar, resets,
     selects the port, and opens. If the open fails, it shows the existing
     warning and calls `on_done(false)`;
  2. disables the port list and refresh button, as today, and also the
     log-transport combo and the Connect and Logging actions, so that no UI
     path touches the facade while the worker holds it;
  3. for make `"Subaru"`, it maps the log transport to an `SsmVariant`
     (`SSM` → `Ssm1`, `K-Line` → `KlineSsm2`, `iso15765` → `Iso15765Uds`),
     reads the ECU/TCU radio button for the target, and starts
     `SsmIdentifyWorker`. For `CAN` it starts nothing, which preserves the
     dead branch. On `completed` it re-enables the controls. On success it
     sets `ecuid` and `ecu_init_complete` and the status bar, and calls
     `parse_log_value_list(init_response, "SSM")` when `init_response` is
     not empty. On failure it calls `disconnect_from_ecu()`. Either way it
     then calls `on_done(true)`: a port that opened counts as a successful
     connect even when identification failed (pinned);
  4. for any other make it calls `disconnect_from_ecu()` and then
     `on_done(true)` immediately, without the 2.5 s of empty retries.
- The logging start in `menu_actions.cpp` becomes a continuation:
  `connect_to_ecu([this](bool ok) { if (!ok) { …existing abort… } else continue_start_logging(); })`,
  where `continue_start_logging` is the existing code after the connect
  check, moved into its own function.
- `disconnect_from_ecu`, `closeEvent`, and `start_ecu_operations` cancel and
  join any running worker before they touch the facade.
- The BIU dialog is prepared through the link:
  `link.open(KlineLinkConfig{.header = None, .iso14230_connection = true, .baud = 10400})`
  before `exec()`, and `link.set_header(KlineHeader::None)` after.
- Deleted: `ecu_init`, `ssm_init`, `ssm_kline_init`, `ssm_can_init`,
  `add_ssm_header`, `calculate_checksum`, `parse_ecuid`, and
  `ecu_init_started`. `log_operations_ssm.cpp` keeps `parse_log_value_list`
  and `log_to_file`; the plan decides whether to rename it.

### Tests

- `ssm_identify_test` (`fastecu_portable_gtest`): exact request bytes for
  each variant and target; the read budgets; one rejection test per
  validation field; ECU-ID extraction, including the short frame; the SSM1
  drain choosing the last complete frame; `Timeout` versus `BadResponse`;
  and cancellation in a read and in a sleep. Each validation check is backed
  by a mutation check, as the design notes require.
- `serial_diagnostic_link_test`: parity reaches `set_serial_port_parity`
  before `open_serial_port`.
- `ssm_identify_worker_test`: stops after the first success; retries 5 times
  with 500 ms sleeps; stops at once on `Cancelled`; emits `completed` exactly
  once; the destructor joins a running worker.
- `adapter_connection_test`: the legacy facade call sequence for each member,
  against the fake backend.
- `//src/platform/desktop/common/connection/testing:adapter_connection_harness`
  builds a facade over the fake backend and exposes the backend for
  expectations, so `mainwindow_test` never includes `serial_port_actions.h`.
  Its existing `open_serial_port` expectations move to backend-level
  expectations.
- `mainwindow_test`: connect on a non-Subaru make opens, disconnects, and
  continues without waiting; the controls are disabled while identifying and
  re-enabled afterward; a logging start waits for the continuation.
- `menu_command_test`: the three deleted command names parse as unknown.

## PR sequence

These form a stack with `gh stack`. Each PR is reviewed against its parent.

1. **6h-0: this spec and its implementation plan.**
2. **6h-1: delete the dev toggles.** This is independent of the rest.
3. **6h-2: parity and `identify_ssm_ecu`.** No callers yet.
   `//:portable_closure` stays green.
4. **6h-3: `AdapterConnection`.** `MainWindow`'s lifecycle calls, dialog
   preparation, and BIU preparation go through it; `MainWindowServices` is
   swapped; the test harness lands. The SSM init code still calls the
   facade, through `connection.facade()`, so the header is still included by
   `log_operations_ssm.cpp` in this PR.
5. **6h-4: the worker, asynchronous connect, and close-out.**
   `SsmIdentifyWorker`; the connect continuation; the legacy SSM code is
   deleted; `//src/ui/desktop:__pkg__` leaves `serial_qt_compat` and
   `FROZEN`. Docs: a `## Connection and identification` section in the
   [design notes](../../design-notes.md), the step 6h entry in the
   [modularization plan](../../modularization-plan.md), the `serial_qt_compat`
   entry and new CAN-identification entry in the
   [tech-debt roadmap](../../tech-debt.md), and a new
   `docs/connection-bench-checklist.md`. This spec and its plan are deleted.

## Behavior changes

1. **The dev toggles are gone.** A hand-edited menu naming them gets the
   unknown-command behavior.
2. **SSM identification runs off the UI thread.** The window stays
   responsive, the controls that could reach the facade are disabled until
   identification finishes, and closing or disconnecting cancels it.
   Cancellation is best-effort, in reads and sleeps, as with `DtcWorker`.
3. **SSM2 K-Line init responses are validated.** A frame with a wrong
   header, ID, length byte, response code, or checksum is rejected and
   retried, where today it produces a wrong ECU ID and wrong log
   capabilities.
4. **SSM1's two plain writes are echo-checked.**
5. **K-Line SSM2 identification opens the link itself**, with the settings
   it previously inherited.
6. **BIU opens directly at 10400**, instead of opening at the previous speed
   and then changing it.
7. **Non-Subaru connect skips 2.5 s of empty retries.** This includes every
   Mitsubishi MUT/DMA logging start.
8. **Short init frames fail cleanly.** They become `BadResponse`, where the
   legacy code read out of range.

Unchanged by design and pinned by tests: a connect succeeds when the port
opens, even if identification fails; the raw-`CAN` identification branch
does nothing; SSM1 is checked only by length; CAN identification uses
UDS `22 F1 82`; and a port refresh leaves parity unchanged.

## Risk and verification

Every PR runs `bazel build --config=release //:fastecu`,
`bazel test --config=release //...`, `prek run --all-files`, and
`bazel run //:clang_tidy_report_changed`. The allowlist removal proves
itself: `//:serial_compat_allowlist` passes with the shorter list, and any
`//src/ui/desktop` file that still includes the header fails to build.

The highest-risk changes are behavior changes 4 and 7: SSM1 echo handling,
which is untested on hardware in this fork, and the Mitsubishi logging start,
which is this project's main bench path. No path is marked qualified until
`docs/connection-bench-checklist.md` records it. That checklist covers:

- SSM2 K-Line identification on an ECU and a TCU, including a validation
  rejection appearing in the log
- SSM1 identification on one of the two `SSM`-transport vehicles
- iso15765 `22 F1 82` identification
- a Mitsubishi MUT/DMA logging start
- BIU at 10400
- DTC and DataTerminal on the selected port
- port refresh and transport switching
- OpenPort battery voltage during a flash
- cancelling identification by disconnecting and by closing the window
