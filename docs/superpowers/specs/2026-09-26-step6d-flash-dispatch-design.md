# Step 6d: extract flash-operation dispatch from `MainWindow`

## Goal

Advance the step 6 bullet "Keep `MainWindow` and dialogs responsible only for
presentation, input collection, signal wiring, and calling backend use cases"
of the [modularization plan](../../modularization-plan.md) by moving flash
dispatch out of `MainWindow::start_ecu_operations`.

Success means:

- `start_ecu_operations` shrinks from 291 lines to roughly half, and
  contains no `goto`. Write preflight and the calibration handoff stay by
  design, so it cannot get much smaller in this step.
- Denso TCU routing, workflow creation, the `FlashDialog`, and the
  unknown-protocol warning live in a `FlashOperationController` that can be
  tested without a `MainWindow`.
- The pure decisions (command parsing, Denso TCU detection, kernel path,
  read-image file name) are portable and gtest-covered.
- `mainwindow.h` no longer includes `flash_dialog.h`.
- No wire sequence, dialog text, or prompt order changes, except for the two
  defect fixes listed under "Behavior changes".

## Current state (2026-09-26, `master` at `db3ac4b2`)

`MainWindow::start_ecu_operations(const QString& cmd_type)`
(`src/ui/desktop/mainwindow.cpp`, 291 lines):

1. Stops realtime logging; returns early with a warning if no serial port
   is selected; sets the serial port list; appends `/` to the kernel
   directory if missing.
2. Only when the selected make is `Subaru` or `Mitsubishi`:
   1. resets the serial facade to an idle line state (seven setters), clears
      `ecuid`/`ecu_init_complete`, reads battery voltage and starts
      `vbatt_timer`;
   2. **write/test_write**: requires a selected calibration ("No file
      selected!" returns), keeps a copy of `FullRomData`, warns when there is
      no checksum module (Cancel returns), fills `RomInfo`/`FlashMethod`/
      `Kernel`/`KernelStartAddr`/`McuType`, runs checksum correction;
   3. **read**: `ecuCalDef[ecuCalDefIndex] = new EcuCalDefStructure`, pads
      `RomInfo`, sets the flash method, calls `update_protocol_info`, fills
      kernel/MCU fields;
   4. maps `cmd_type` to `FlashOperation` (`write`, `test_write`, anything
      else `Read`) and builds the portable image;
   5. for a read on `sub_tcu_denso_sh7055_can`/`sub_tcu_denso_sh7058_can`,
      shows the TCU chooser, logs the choice, and runs the service action;
      when that handles the request, `goto ecu_operation_cleanup`;
   6. builds a `FlashWorkflowRequest` and runs `FlashWorkflowFactory`; with a
      workflow, runs a `FlashDialog` (logs wired to `MainWindow` and the
      syslogger) and copies back read bytes and ROM id; without one, shows
      "Unknown flashmethod";
   7. after a read with data: names the file (`<RomId><timestamp>.bin` or
      `read_image_<timestamp>.bin`), opens it, prompts for a missing
      definition, builds the calibration trees, increments
      `ecuCalDefIndex`, and saves; after a write: restores the saved
      `FullRomData`.
3. `ecu_operation_cleanup:` stops `vbatt_timer`, resets the serial facade to
   idle again, clears `ecuid`/`ecu_init_complete`, re-emits the log
   transport selection, and sets the port speed to 4800.

`mainwindow_test` characterizes the routing through this function: Denso TCU
chooser outcomes, future Denso suffixes, and portable-before-legacy dispatch.

## Design

### Portable helpers

New target `//src/backend/flash:flash_operation_request`
(`flash_operation_request.{h,cpp}` + `_test.cpp`), registered by name under
`src/backend/flash` in `PORTABLE_PACKAGES`:

```cpp
namespace fastecu::flash
{
// "write" -> Write, "test_write" -> TestWrite, anything else -> Read.
FlashOperation flash_operation_from_command(std::string_view command);

// sub_tcu_denso_sh7055_can and sub_tcu_denso_sh7058_can, exact match.
bool is_denso_tcu_protocol(std::string_view protocol);

// dir + file, inserting '/' when dir does not already end with one.
std::string kernel_path(std::string_view dir, std::string_view file);

// "<rom_id><timestamp>.bin", or "read_image_<timestamp>.bin" for an empty rom_id.
std::string read_image_filename(std::string_view rom_id, std::string_view timestamp);
}
```

`MainWindow` keeps formatting the timestamp with `QDateTime` and passes it in.

### `reset_serial_to_idle`

`fastecu::desktop::serial::reset_serial_to_idle(SerialPortActions& serial)`
lives in a new target, `//src/platform/desktop/common/serial:serial_idle`,
visible only to `//src/ui/desktop`. It calls facade methods, so it needs
`SerialPortActions`' definition, and `serial_qt_compat`'s frozen visibility
list may not gain the new UI package. It performs, in order,
`reset_connection`, `set_is_iso14230_connection(false)`,
`set_is_29_bit_id(false)`, `set_add_iso14230_header(false)`,
`set_is_can_connection(false)`, `set_is_iso15765_connection(false)`,
`set_serial_port_parity(QSerialPort::NoParity)`,
`set_serial_port_baudrate("4800")` — exactly the sequence both sites use
today. `ecuid`/`ecu_init_complete` stay `MainWindow` members, cleared by
`MainWindow` next to each call.

### `FlashOperationController`

A new package, `src/ui/desktop/flash/operation`, holds
`FlashOperationController` (`QObject`). It only passes `SerialPortActions*`
through to `FlashWorkflowFactory` and the Denso TCU service functions, so a
forward declaration suffices and it needs no `serial_qt_compat` edge.

```cpp
struct FlashOperationInput
{
    FlashOperation operation;
    std::string protocol;
    std::string mcu;
    std::string kernel_path;             // for the TCU "Dump" log line
    std::optional<bytes::Bytes> image;
    config::ConfigPaths paths;
    std::string display_filename;
};

enum class FlashOperationStatus
{
    Completed,            // a workflow ran; read_bytes/rom_id as the dialog returned them
    ServiceActionHandled, // a Denso TCU service action consumed the request
    Unsupported,          // no workflow for this protocol; warning shown
};

struct FlashOperationOutcome
{
    FlashOperationStatus status;
    std::optional<bytes::Bytes> read_bytes;
    std::optional<std::string> rom_id;
};

class FlashOperationController : public QObject
{
    Q_OBJECT
  public:
    FlashOperationController(SerialPortActions& serial, QWidget *dialog_parent);
    FlashOperationOutcome run(const FlashOperationInput& input);
  signals:
    void LOG_E(QString, bool, bool);  // and LOG_W, LOG_I, LOG_D
    void external_logger(QString);
    void external_logger(int);
};
```

`run` does steps 2.5 and 2.6 above. It forwards the `FlashDialog`'s
`external_logger` overloads and `LOG_*` signals through its own, and
`MainWindow` connects the controller's signals to `external_logger`,
`external_logger_set_progressbar_value`, and the syslogger. The "Unknown
flashmethod" message box keeps its title and text and uses
`dialog_parent`.

### What stays in `MainWindow::start_ecu_operations`

The port check; kernel-directory normalization; a scope-exit cleanup
(below); the make check with the entry reset and battery polling; write
preflight and checksum correction (they mutate the selected calibration and
call `MainWindow` members); read-slot preparation; building
`FlashOperationInput`; `controller.run()`; writing back `read_bytes`/`rom_id`;
the post-read calibration handoff; the post-write `FullRomData` restore.

The cleanup is a scope-exit object constructed right after the port check
and kernel-directory normalization — the point the old `goto` target
covered for every path that reached it. It runs the old cleanup block
verbatim, using `reset_serial_to_idle`.

### Behavior changes

Two defects are fixed; everything else is preserved.

1. **Write-path early returns now clean up.** "No file selected!" and Cancel
   on the checksum warning return after the entry reset and after
   `vbatt_timer` started, but skip the cleanup today, leaving battery
   polling running and the serial facade un-restored. With the scope-exit
   cleanup they now stop polling and restore the idle line state, like
   every other exit.
2. **The read slot no longer leaks.** The read path still allocates
   `ecuCalDef[ecuCalDefIndex]` before dispatch (so `update_protocol_info`
   runs exactly as today), but on any outcome other than a successful read
   with data — cancel, failure, `Unsupported`, `ServiceActionHandled` — the
   slot is deleted and set back to `nullptr`, its state before the first
   read. Only `start_ecu_operations` reads that slot before
   `ecuCalDefIndex` advances.

The early return for "No serial port selected!" still happens before the
cleanup object exists, as today.

## Non-goals

- Changing write preflight, checksum correction, or the calibration handoff.
- Changing `FlashWorkflowFactory`, `FlashDialog`, or the Denso TCU service
  functions.
- Replacing `ecuCalDef`'s raw fixed array (tracked under "Replace
  parallel-list data models").
- Any other `serial->` call site, logging, or platform selection.

## Testing

### Prerequisite: `test_mainwindow` must fail on Google Mock violations

`mainwindow_test.cpp`'s `main` never calls `InitGoogleMock` and returns only
QtTest's result, so Google Mock expectation failures print but never fail
the target. On `master` (and before step 6c) its Denso TCU case reports 14
such failures and still passes: the case expects no `reset_connection` or
`change_port_speed` calls on a cancelled chooser, one of each in sequence on
a declined relearn, and one `read_vbatt`/`set_use_openport2_adapter` call —
none of which happens. The real calls are three `reset_connection` and two
`change_port_speed("4800")` in both rows, and no `read_vbatt` or
`set_use_openport2_adapter`. Step 6d relies on this test, so it first adopts
the repo's existing pattern (`InitGoogleMock`, then fail when
`::testing::Test::HasFailure()`) and pins the observed counts.


- `//src/backend/flash:flash_operation_request_test` (gtest): every command
  mapping including unknown strings; both TCU protocol names plus
  near-misses (the future suffixes `mainwindow_test` already uses);
  `kernel_path` with and without a trailing `/` and with an empty file;
  both `read_image_filename` forms.
- `//src/ui/desktop/flash/operation:flash_operation_controller_test`
  (QtTest, offscreen, fake serial backend): an unknown protocol returns
  `Unsupported`, shows the warning, and performs no serial I/O; a Denso TCU
  read whose chooser is cancelled returns `ServiceActionHandled` with no
  I/O.
- `//src/platform/desktop/common/serial:serial_idle_test` (QtTest, fake
  serial backend): `reset_serial_to_idle` issues the eight calls in order.
- `//src/ui/desktop:test_mainwindow`: all existing cases pass unchanged,
  plus one case per behavior change — the checksum-warning Cancel stops
  `vbatt_timer` and resets the serial facade; a read of an unsupported
  protocol leaves `ecuCalDef[ecuCalDefIndex] == nullptr`.
- `bazel test --config=release //...` on all three CI platforms. No bench
  checklist is affected: no wire sequence changes.

## Delivery

Four PRs as a `gh stack`, each green on its own:

1. **6d-1** — `test_mainwindow` enforces Google Mock and pins the Denso TCU
   counts; the portable helpers, adopted at their `MainWindow` call sites.
2. **6d-2** — `serial_idle` and the scope-exit cleanup: removes the
   `goto` and fixes behavior change 1.
3. **6d-3** — `FlashOperationController`, the switch in `MainWindow`,
   behavior change 2, and dropping `flash_dialog.h` from `mainwindow.h`.
4. **6d-4** — docs: mark progress in the modularization plan, remove the
   fixed `goto` item from the tech-debt roadmap, distill this spec into the
   design notes, and delete the spec and plan.
