# Step 6c: desktop composition root

## Goal

Deliver the step 6 bullet "Move construction and platform selection into
`apps/desktop`" of the [modularization plan](../../modularization-plan.md).
`MainWindow` stops constructing the long-lived desktop services; a
composition root in `apps/desktop` builds and owns them and injects them.
Desktop behavior is unchanged: no wire path, dialog flow, or config format
changes.

Success means:

- `apps/desktop/main.cpp` constructs every long-lived service through
  `DesktopComposition` and passes them to `MainWindow`.
- `MainWindow` no longer calls `new`/`make_unique` for any of the services
  listed below.
- `mainwindow_test` injects its fake-backed serial facade through the
  constructor instead of overwriting `window.serial`.
- The dead `EcuOperations` class is gone.

## Current state (2026-09-26, `master` at `2f9a7406`)

`MainWindow::MainWindow(peerAddress, peerPassword, parent, config_root)`
(`src/ui/desktop/mainwindow.cpp`) does, in order:

1. Builds `FileActions` over member `QtFileSystem`, `QtResourceBundle`,
   `QtFileRepository`, `QtAtomicFileWriter`, and `QtEventSink`, then calls
   `set_base_dirs(config_root or the configured base directory)`.
2. Starts `SystemLogger` on a new `QThread` using
   `configValues->syslog_files_directory`.
3. `setupLoggingEngine()`: constructs `LoggingEngine(this)` and registers the
   `MUT_DMA`, `CDBG`, and `SSM` protocol factories, which capture `serial`
   and (SSM) `ecu_radio_button`.
4. Reads config and protocols, builds menus, opens remembered calibrations,
   builds the network splash.
5. `serial = new SerialPortActions(peer, password, nullptr, this)` and
   `remote_utility = new RemoteUtility(peer, password, nullptr, this)`; in
   remote mode blocks in `waitForSource()` on both behind the splash.

`Settings` (constructed on the stack in `menu_actions.cpp`) keeps its own
three config adapters and builds a throwaway `FileActions` in every
`save_config_file()`.

`src/ui/desktop/ecu_operations.{h,cpp}` (2,315 lines) define `EcuOperations`,
which is compiled but never instantiated; the only reference is an
unassigned `EcuOperations *ecuOperations` member in
`get_key_operations_subaru.h`, used only in a commented-out line.

## Design

### Ownership

A new `DesktopComposition` class owns, declared in this order:

1. `QtFileSystem`, `QtResourceBundle`, `QtFileRepository`,
   `QtAtomicFileWriter`, `QtEventSink`
2. `FileActions` (with `set_base_dirs` already applied)
3. `SystemLogger` and its thread (started)
4. `SerialPortActions`
5. `RemoteUtility`
6. `QtClock`
7. `LoggingEngine`

Reverse declaration order gives the required destruction order: the engine
dies before the serial facade its transports reference, and both die before
`FileActions` and the adapters.

In `main()`, the composition is declared before `MainWindow` inside the
existing `RESTART_CODE` loop, so both are rebuilt per restart as today and
the composition outlives the window. `~MainWindow`'s
`loggingEngine->stop()` therefore still runs against live transports.

The one lifetime change: `SerialPortActions` and `RemoteUtility` were Qt
children of `MainWindow` and died in `~QObject`; they now die after
`MainWindow`, when the composition is destroyed. They are constructed with a
null parent.

### `MainWindowServices`

Declared in `src/ui/desktop/main_window_services.h` — the consumer owns the
type. A non-owning aggregate of references:

```cpp
struct MainWindowServices
{
    FileActions& file_actions;
    QtFileRepository& config_repository;
    QtEventSink& file_action_events;
    SystemLogger& syslogger;
    SerialPortActions& serial;
    RemoteUtility& remote_utility;
    fastecu::desktop::logging::LoggingEngine& logging_engine;
    QtClock& logging_clock;
};
```

`MainWindow`'s constructor becomes
`MainWindow(MainWindowServices services, const QString& peerAddress, QWidget *parent = nullptr)`.
`peerPassword` and `config_root` leave the signature: the password is
consumed only by the facades' construction, and `set_base_dirs` moves to the
composition. `peerAddress` stays because the network splash displays it.

Existing members keep their names so the call sites are untouched: `serial`,
`remote_utility`, `loggingEngine`, and `syslogger` become non-owning
pointers initialized from the services; `fileActions` changes from
`std::unique_ptr<FileActions>` to `FileActions *`. The member adapters
`m_configFileSystem`, `m_configResourceBundle`, `m_configFileRepository`,
`m_definitionFileWriter`, `fileActionsEvents_`, and `m_loggingClock` are
removed; code that used them reads the injected references.

The struct grows across the PRs: 6c-2 introduces it with the fields for the
objects that PR moves (`file_actions`, `config_repository`,
`file_action_events`, `syslogger`), and 6c-3 adds the rest.

### `DesktopComposition`

A `qt_cc_library` `//apps/desktop:composition`
(`apps/desktop/desktop_composition.{h,cpp}`), private to `apps/desktop`:

```cpp
class DesktopComposition
{
  public:
    DesktopComposition(const QString& peerAddress, const QString& peerPassword, const QString& config_root = {});
    ~DesktopComposition();
    MainWindowServices services();
};
```

It connects the `LOG_E/W/I/D` signals of `SerialPortActions` and
`LoggingEngine` to `SystemLogger::log_messages`, and starts the syslogger
thread, as `MainWindow` does today. `config_root` empty means "use the
configured base directory", as today.

Its destructor releases dependents first — engine, remote utility, serial
facade — then quits and joins the syslogger thread and deletes the logger.
Today the syslogger and its thread are never stopped: the
`SystemLogger::finished` signal the old wiring waits on is never emitted, so
each `RESTART_CODE` iteration leaks a running thread. Stopping it is the one
deliberate behavior change in this step.

`main.cpp` becomes: parse arguments, construct `QApplication`, construct
`DesktopComposition`, construct `MainWindow{composition.services(), addr}`,
center and show, `exec`, loop on `RESTART_CODE`.

### Reaching the serial facade from `apps/desktop`

`SerialPortActions` lives in `serial_qt_compat`, whose visibility list is
frozen and shrink-only (`scripts/check-serial-compat-allowlist.py`);
`apps/desktop` is not on it and must not be added. A new target in the same
package, `//src/platform/desktop/common/serial:desktop_serial_factory`,
visible only to `apps/desktop`, exposes construction and nothing else:

```cpp
struct SerialPortActionsDeleter
{
    void operator()(SerialPortActions *serial) const;
};
using OwnedSerialPortActions = std::unique_ptr<SerialPortActions, SerialPortActionsDeleter>;

OwnedSerialPortActions make_serial_port_actions(const QString& peer_address, const QString& peer_password,
                                                QObject& log_sink);
```

`SerialPortActions` stays an incomplete type in `apps/desktop`: the
composition only binds a reference to it for `MainWindowServices`. The
factory routes the facade's `LOG_*` signals to `log_sink`'s
`log_messages(QString, bool, bool)` slot, so the composition never needs the
facade's declaration.

### What stays in `MainWindow`

- The startup splash, config/protocol reading, menu building, and the
  network splash including `waitForSource()` on the injected `serial` and
  `remote_utility`. Remote-mode startup looks identical.
- Signal wiring whose receiver or sender is `MainWindow` itself: the
  `QtEventSink::logged`/`noticed` handlers (they re-emit `MainWindow`'s own
  `LOG_*` signals), `LoggingEngine::valuesUpdated`/`sessionEnded`, and
  `stateChanged` → `network_state_changed`.
- Logging-protocol registration (`setupLoggingEngine()`, minus the
  construction and the `LOG_*` connections). The SSM factory reads
  `ecu_radio_button`, which does not exist until the window does; moving
  registration would need a callback from the window back into the
  composition. Recorded as a follow-up in the tech-debt roadmap.

### `Settings`

`Settings(FileActions& fileActions, FileActions::ConfigValuesStructure *configValues, QWidget *parent)`.
It drops its three adapters, its `QtAtomicFileWriter`, its event sink, and
the throwaway `FileActions`; `save_config_file()` calls
`fileActions.save_config_file(configValues)`. The one construction site in
`menu_actions.cpp` passes `*fileActions`. The throwaway instance reported to
a `NullEventSink`; the shared one reports to `MainWindow`'s sink, so any
diagnostic the save emits now reaches the log window instead of being
dropped.

### Dead code

Delete `src/ui/desktop/ecu_operations.{h,cpp}`, their `BUILD.bazel`
entries, the `ecuOperations` member and include in
`get_key_operations_subaru.h`, and the commented-out uses. Keep
`ecu_operations.ui`: `FlashDialog` and the get-key dialog use its generated
`Ui::EcuOperationsWindow`.

## Error handling

No new failure paths. Construction does no fallible work beyond what
`FileActions` already reports through the event sink, which is unchanged.
The composition's constructor does not throw on missing config; behavior on
a missing or unreadable config directory is whatever `set_base_dirs` does
today.

## Non-goals

- Changing any `serial->` call site or the `serial_qt_compat` allowlist.
- Removing the three grandfathered `ui → platform` BUILD edges; the injected
  types are the same platform types.
- Flash-operation dispatch (`start_ecu_operations`), `FileActions` model
  types, and logging-protocol registration.
- Any change to the network splash, restart prompt, or startup splash
  sequence.

## Testing

- `//src/ui/desktop:test_mainwindow` builds its own services from real Qt
  adapters, a temporary config root, and a `SerialPortActions` over
  `NiceFakeBackend`, and passes them to the new constructor. The three
  `window.serial = serial.release()` overwrites go away; its
  `#define private public` stays for its other private accesses.
- New `//apps/desktop:desktop_composition_test` (`fastecu_qttest`,
  `QTemporaryDir` config root, direct-mode serial facade as the old
  `MainWindow` tests already constructed): `services()` returns stable
  references to the composition's own objects; destroying it immediately
  after construction, and constructing it twice in one process as the restart
  loop does, completes without crash or hang.
- New `//src/platform/desktop/common/serial:desktop_serial_factory_test`:
  each `LOG_*` level reaches the sink's `log_messages` slot.
- New `//src/ui/desktop:test_settings`: destroying `Settings` writes the
  config file through the injected `FileActions`.
- `bazel test --config=release //...` green on all three CI platforms.
- Manual smoke on macOS: start the packaged app in local mode, open and
  close Settings (config saved), and quit; start with `-s` against a running
  remote utility and confirm the network splash and connection.
- No bench checklist is affected: no wire path changes.

## Delivery

Four PRs, each green on its own:

1. **6c-1** — delete dead `EcuOperations`.
2. **6c-2** — add `MainWindowServices` and `DesktopComposition`; move the
   adapters, `FileActions`, and `SystemLogger`; `Settings` takes
   `FileActions&`. `MainWindow` still builds serial, remote utility, and
   the engine. The constructor changes here, so `mainwindow_test` switches
   to passing services in this PR; its `window.serial` overwrites remain
   until 6c-3.
3. **6c-3** — add `desktop_serial_factory`; move `SerialPortActions`,
   `RemoteUtility`, `LoggingEngine`, and `QtClock` into the composition; drop the `window.serial` overwrites from
   `mainwindow_test`; add `composition_test`.
4. **6c-4** — docs: mark the step 6 construction bullet complete in the
   modularization plan, update the "P1: Separate UI from application logic"
   entry in the tech-debt roadmap, record the protocol-registration
   follow-up, and distill this spec into the design notes.
