# Step 6j: UI channels for the system log and the remote utility

## Goal

Finish the "Remove the GRANDFATHERED UI → platform edges (step 6j)" entry in
the [tech-debt roadmap](../../tech-debt.md) and the step 6j bullet in the
[modularization plan](../../modularization-plan.md).

Success means:

- No `//src/ui/desktop` target depends on
  `//src/platform/desktop/common/logging:logging` (`SystemLogger`) or
  `//src/platform/desktop/common/remote_utility` (`RemoteUtility`), and no UI
  source includes `systemlogger.h` or `remote_utility.h`.
- No `GRANDFATHERED` visibility entry remains anywhere in the tree.
- Log levels, the log-window filter, file logging, the startup network wait,
  the "connection lost" dialog, and the remote log/progress mirror behave as
  they do today, with one deliberate exception (see
  [Behavior change](#behavior-change)).
- Only UI, composition-root, BUILD, test, and documentation files change. No
  ECU I/O path changes, so there is no bench checklist.

## Non-goals

- Changing `SystemLogger` or `RemoteUtility` themselves. Both classes keep
  their current API and behavior.
- Moving logging onto the backend `IEventSink` port. `IEventSink::log` has no
  timestamp or linefeed flags, which the UI's `LOG_*` call sites (about 150 in
  `mainwindow.cpp` and `menu_actions.cpp`) rely on.
- Changing the `LOG_*` signals that UI classes, dialogs, and flash classes
  declare. They keep their `(QString, bool, bool)` shape.
- Changing how the composition root connects `LoggingEngine` and the serial
  facade to `SystemLogger`. Those are platform-to-platform edges inside
  `apps/desktop`.
- Cutting a portable port for either service. As with `AdapterConnection`
  ([design notes](../../design-notes.md#adapterconnection-is-a-concrete-adapter-not-a-port)),
  Android's first target needs neither; a port can be cut later if one does.

## Current state (2026-09-27, `master` at `db2b66f3`)

Two platform packages list `//src/ui/desktop:__pkg__` in their
`default_visibility` under a `GRANDFATHERED` comment:
`//src/platform/desktop/common/logging` and
`//src/platform/desktop/common/remote_utility`. `MainWindowServices` hands
`MainWindow` a `SystemLogger&` and a `RemoteUtility&`, which
`DesktopComposition` owns.

### `SystemLogger`

`mainwindow.h` includes `systemlogger.h`. Every UI use is a connection:

- `MainWindow`'s constructor connects its own `LOG_E/W/I/D` to
  `SystemLogger::log_messages`, its `enable_log_write_to_file` to the slot of
  the same name, and `SystemLogger::send_message_to_log_window` back to
  `MainWindow::send_message_to_log_window`.
- `MainWindow`'s constructor also connects `DefinitionAuthoringDialog` and
  `CalibrationTreeWidget` `LOG_*` to `log_messages`.
- `connect_signals_and_run_module` connects a flash class's `LOG_*` to
  `log_messages`.
- `menu_actions.cpp` connects the stack-local `DtcOperations`,
  `BiuOperationsSubaru`, and `DataTerminal` dialogs' `LOG_*` to
  `log_messages`.

`SystemLogger` lives on its own thread, so every one of these connections is
queued. `log_messages` derives the level from the sender's signal name
(`sender()->metaObject()->method(senderSignalIndex()).name()`), prefixes
`(EE)`/`(WW)`/`(II)`/`(DD)` when `timestamp` is set, forwards every non-`LOG_D`
line to `send_message_to_log_window`, and returns early when `sender()` is
null. A queued call whose sender was destroyed before delivery arrives with a
null sender and is dropped. That is why the flash controller already relays
through `MainWindow`'s own `LOG_*` signals (`mainwindow.cpp`, "Relay through
MainWindow's own LOG_* signals").

### `RemoteUtility`

`mainwindow.h` includes `remote_utility.h`. `MainWindow` uses it four ways:

1. `waitForSource()`, a blocking loop, during the network splash when a peer
   address is set.
2. `stateChanged(QRemoteObjectReplica::State, QRemoteObjectReplica::State)`,
   connected with `Qt::DirectConnection` to `network_state_changed`, which
   raises the "Network connection lost" dialog.
3. `external_logger(QString)` calls `send_log_window_message` when
   `isValid()`.
4. `external_logger_set_progressbar_value(int)` calls `set_progressbar_value`
   when `isValid()`.

### The package-wide entry

The logging package's `GRANDFATHERED` entry is in `default_visibility`, so it
also covers `logging_runtime` (`LoggingEngine`, which `MainWindowServices`
carries) and `logging_adapters` (whose headers `mainwindow.h` includes).
Removing the entry without replacing it would cut those edges too.

### Tests

`//src/ui/desktop:test_mainwindow` builds a real `SystemLogger` and a
`RemoteUtility{"", ""}` in its `TestServices` fixture and depends on both
platform targets. `//apps/desktop:desktop_composition_test` checks that
`services()` returns stable references, including `syslogger` and
`remote_utility`.

## Design

The UI declares what it needs as two signal-only `QObject`s it owns. The
composition root connects them to the platform objects. The UI names no
platform logging or remote type.

### Package `//src/ui/desktop/channels`

A new package with two `qt_cc_library` targets, `log_channel` and
`remote_peer`, each a single moc'd header with no `.cpp` beyond what moc
needs. Package visibility: `//src/ui/desktop:__subpackages__` and
`//apps/desktop:__pkg__`. Deps: `QT_DEPS` only.

```cpp
namespace fastecu::ui
{

// The UI's system-log endpoint. The composition root connects it to the
// desktop system logger. The LOG_* names are part of the contract: the
// logger reads the level from the name of the signal that delivered a line.
class LogChannel final : public QObject
{
    Q_OBJECT

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);
    void enable_log_write_to_file(bool enable);
    // Formatted non-debug lines for the log window, from the logger.
    void log_window_message(QString message);
};

// The remote utility peer as MainWindow sees it. The composition root
// connects it to the remote utility replica.
class RemotePeer final : public QObject
{
    Q_OBJECT

  public:
    // Blocks until the peer's source is available. wait_requested is
    // direct-connected, so the wait runs inside this call.
    void wait_for_source()
    {
        emit wait_requested();
    }

  signals:
    void wait_requested();
    // Mirrored to the peer when its replica is valid; dropped otherwise.
    void log_window_message(QString message);
    void progress(int value);
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State old_state);
};

} // namespace fastecu::ui
```

`remote_peer.h` includes `<QRemoteObjectReplica>` for the state enum, as
`adapter_connection.h` does.

### `MainWindowServices`

`SystemLogger& syslogger` becomes `fastecu::ui::LogChannel& log`, and
`RemoteUtility& remote_utility` becomes `fastecu::ui::RemotePeer& remote`.
The forward declarations of `SystemLogger` and `RemoteUtility` are removed.

### UI side

- `mainwindow.h` drops the `systemlogger.h` and `remote_utility.h` includes
  and includes `<QRemoteObjectReplica>` for `network_state_changed`'s
  parameters. The `SystemLogger *syslogger` and
  `RemoteUtility *remote_utility` members become `LogChannel *log_channel` and
  `RemotePeer *remote_peer`.
- Each `QObject::connect(x, &X::LOG_E, syslogger, &SystemLogger::log_messages)`
  becomes the signal-to-signal
  `QObject::connect(x, &X::LOG_E, log_channel, &LogChannel::LOG_E)`, and
  likewise for `W`, `I`, and `D`. This covers `MainWindow`'s own signals,
  `DefinitionAuthoringDialog`, `CalibrationTreeWidget`,
  `connect_signals_and_run_module`, and the three `menu_actions.cpp` dialogs.
- `MainWindow::enable_log_write_to_file` connects to
  `LogChannel::enable_log_write_to_file`, and
  `LogChannel::log_window_message` connects to
  `MainWindow::send_message_to_log_window`.
- The network splash calls `remote_peer->wait_for_source()`.
- `RemotePeer::stateChanged` connects to `network_state_changed` with
  `Qt::DirectConnection`, as before.
- `external_logger` emits `remote_peer->log_window_message(message)` and
  `external_logger_set_progressbar_value` emits `remote_peer->progress(value)`.
  Neither checks validity; that moves to the composition root.

### Composition side

`DesktopComposition` owns a `LogChannel` and a `RemotePeer` and wires them in
its constructor, before any `MainWindow` exists:

```cpp
// system log
connect(&log_channel_, &LogChannel::LOG_E, syslogger_.get(), &SystemLogger::log_messages);
// ... LOG_W, LOG_I, LOG_D
connect(&log_channel_, &LogChannel::enable_log_write_to_file,
        syslogger_.get(), &SystemLogger::enable_log_write_to_file);
connect(syslogger_.get(), &SystemLogger::send_message_to_log_window,
        &log_channel_, &LogChannel::log_window_message);

// remote utility
connect(&remote_peer_, &RemotePeer::wait_requested,
        remote_utility_.get(), &RemoteUtility::waitForSource, Qt::DirectConnection);
connect(&remote_peer_, &RemotePeer::log_window_message, remote_utility_.get(),
        [ru = remote_utility_.get()](const QString& m) { if (ru->isValid()) ru->send_log_window_message(m); });
connect(&remote_peer_, &RemotePeer::progress, remote_utility_.get(),
        [ru = remote_utility_.get()](int v) { if (ru->isValid()) ru->set_progressbar_value(v); });
connect(remote_utility_.get(), &RemoteUtility::stateChanged,
        &remote_peer_, &RemotePeer::stateChanged);
```

The lambdas use the remote utility as their context object, so they
disconnect when it is destroyed. The channels are value members; the
destructor keeps its existing explicit order (logging engine, remote utility,
serial facade, then the syslog thread), and `QObject` disconnects any
connection whose endpoint is gone.

`SystemLogger` now sees `LogChannel` as the sender of every UI line, and
`LogChannel` emits the line on a signal named `LOG_E`/`LOG_W`/`LOG_I`/`LOG_D`,
so the level prefix and the log-window filter are unchanged.

### Behavior change

Before 6j, a line that a stack-local dialog (DTC, BIU, DataTerminal) or a
quickly destroyed flash class queued to the syslog thread just before it was
destroyed arrived with a null `sender()` and was silently dropped. After 6j
the sender is the long-lived `LogChannel`, so those lines are logged. This is
a fix, recorded in the design notes. No ECU I/O changes.

### Visibility

- `//src/platform/desktop/common/logging`: the `GRANDFATHERED` entry leaves
  `default_visibility`. `logging_runtime` and `logging_adapters` get a
  target-level `visibility` of the package default plus
  `//src/ui/desktop:__pkg__`, commented as UI-facing adapters in the way
  `adapter_connection` is. The `logging` target (`SystemLogger`) is visible
  to `//apps/desktop`, `//src/platform:__subpackages__`, and `//tests` only.
- `//src/platform/desktop/common/remote_utility`: the `GRANDFATHERED` entry is
  removed.
- `//src/ui/desktop:desktop` and `:test_mainwindow` drop their deps on
  `common/logging:logging` and `remote_utility` and gain
  `//src/ui/desktop/channels:log_channel` and `:remote_peer`.

Adding a UI dependency on either target now fails at analysis with a
visibility error. The proof, done once by hand and recorded in the PR: a
temporary `deps` entry on `common/logging:logging`, then on
`remote_utility`, from `//src/ui/desktop:desktop` fails `bazel build` with a
visibility error. Unlike the 6i header-reach proof, this is a direct-dep
visibility check, so it holds on Windows too.

### CLAUDE.md

The "Ratchet lists only shrink" bullet names "the GRANDFATHERED
`//src/ui/desktop` entries in platform packages' `default_visibility`". After
6j that list is empty. The bullet is reworded to name only the `qt_layer`
group, and the layering section gains the rule that a new `ui → platform`
edge must go to a target designed as a UI-facing adapter, with its own
target-level visibility, never through a package's `default_visibility`.

## Testing

### `//apps/desktop:desktop_composition_test`

The stable-references case switches to `log` and `remote`. New cases, which
pin the wiring that moved out of the UI:

1. `LogChannel::LOG_E`, `LOG_W`, and `LOG_I` with `timestamp = true` each
   arrive at `LogChannel::log_window_message` with the `(EE) `, `(WW) `, or
   `(II) ` prefix, across the syslog thread (`QTRY_*`).
2. `LogChannel::LOG_D` does not arrive at `log_window_message`.
3. A line relayed through `LogChannel` from a sender `QObject` that is
   destroyed immediately after emitting still arrives. This pins the
   behavior change.
4. With an empty peer address, emitting `RemotePeer::log_window_message` and
   `progress` returns without blocking or crashing (the replica is not
   valid, so nothing is sent).

Cases 1 and 3 are pinned the way the
[design notes](../../design-notes.md#pin-every-correction-with-a-mutation-check)
require: swapping the composition's `LOG_E` and `LOG_W` connections turns
case 1 red, and case 3 carries a control assertion that the same destroyed
sender, connected straight to `SystemLogger::log_messages`, loses its line.
Case 4 has no mutation check: with no peer, removing the `isValid` guard leads to a call on an invalid replica, and
what that does is Qt's behavior, not ours to pin.

### `//src/ui/desktop:test_mainwindow`

`TestServices` owns a `LogChannel` and a `RemotePeer` in place of
`SystemLogger` and `RemoteUtility`, and the test target drops both platform
deps. The existing `QSignalSpy` on `MainWindow::LOG_I` stays. One new case: a
line `MainWindow` emits on `LOG_I` reaches `LogChannel::LOG_I` with the same
arguments.

### Channels

No unit tests of their own. They contain no logic beyond
`RemotePeer::wait_for_source` emitting one signal, which the composition wiring
and the network splash exercise.

## Delivery

A `gh stack` of three PRs:

1. **6j-0** (`docs/step6j-ui-channels`): this spec and the implementation
   plan.
2. **6j-1** (`refactor/step6j-1-log-channel`): `LogChannel`, the UI and
   composition changes for `SystemLogger`, the logging package visibility
   split, composition cases 1–3 and their mutation checks, the
   `test_mainwindow` fixture change for the log, and the visibility proof for
   `common/logging:logging`.
3. **6j-2** (`refactor/step6j-2-remote-peer`): `RemotePeer`, the UI and
   composition changes for `RemoteUtility`, composition case 4, the
   `remote_utility` visibility entry, the visibility proof for
   `remote_utility`, and the close-out: the modularization plan's 6j entry
   and Status paragraph, removal of the tech-debt P1 entry, a "UI channels"
   section in the design notes (why the channels are UI-owned, the
   signal-name contract, the dropped-line fix), the CLAUDE.md wording, and
   deletion of this spec and its plan.

Each PR passes `bazel test --config=release //...`, `prek run --all-files`,
and `bazel run //:clang_tidy_report_changed`.
