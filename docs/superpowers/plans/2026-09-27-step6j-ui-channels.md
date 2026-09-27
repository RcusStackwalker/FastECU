# Step 6j: UI Channels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the last two GRANDFATHERED `ui → platform` edges (`SystemLogger`, `RemoteUtility`) by routing `MainWindow` through two UI-owned signal channels that the desktop composition root wires to the platform objects.

**Architecture:** A new package `//src/ui/desktop/channels` holds `LogChannel` and `RemotePeer`, each a signal-only `QObject`. `DesktopComposition` owns them and connects them to `SystemLogger` and `RemoteUtility`; `MainWindowServices` carries them in place of the platform objects. Each edge is done in two steps: first the composition gains and wires the channel alongside the old service (so every commit builds), then the UI moves over and the old service and its visibility entry go.

**Tech Stack:** Bazel 9.1.1 (Bzlmod), `qt_cc_library` / `fastecu_qttest` from `bazel/qt_targets.bzl`, Qt 6 (Core, Widgets, RemoteObjects), QtTest + `QSignalSpy`, prek.

**Spec:** [the step 6j design](../specs/2026-09-27-step6j-ui-channels-design.md)

## Global Constraints

- `SystemLogger` and `RemoteUtility` keep their current API and behavior. Neither `systemlogger.{h,cpp}` nor `remote_utility.{h,cpp,rep}` is edited.
- UI `LOG_*` signals keep their `(QString message, bool timestamp, bool linefeed)` shape.
- The composition root's existing `LoggingEngine → SystemLogger` connections and the serial factory's sink are unchanged.
- The channel package is `//src/ui/desktop/channels`, visibility exactly `["//apps/desktop:__pkg__", "//src/ui/desktop:__subpackages__"]`, deps `QT_DEPS` only.
- A `Q_OBJECT` header is in `hdrs` of exactly one `qt_cc_library`.
- `MainWindowServices` ends as: `file_actions`, `config_repository`, `file_action_events`, `log`, `connection`, `remote`, `logging_engine` (in that order; designated initializers must follow it).
- After Task 4, `grep -rn GRANDFATHERED --include=BUILD.bazel .` (excluding `bazel-*`) prints nothing.
- No bench checklist: no ECU I/O path changes.
- Work lands as a `gh stack`: `docs/step6j-ui-channels` (6j-0, the spec and this plan) → `refactor/step6j-1-log-channel` (Tasks 1–2) → `refactor/step6j-2-remote-peer` (Tasks 3–5).
- Every PR passes `bazel test --config=release //...`, `prek run --all-files`, and `bazel run //:clang_tidy_report_changed`.
- Every commit ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ
  ```

## Review Focus

- **A UI line logged at the wrong level, or a debug line reaching the log window.** Expected: `(EE)`/`(WW)`/`(II)` prefixes as today, `LOG_D` kept out of the window. Pinned by `channelLevelsReachTheLogWindowWithTheirPrefix` and `debugLinesStayOutOfTheLogWindow` (Task 1), with a swap mutation.
- **A line from a dialog destroyed right after logging (DTC, BIU, DataTerminal, a flash class).** Expected: the line is logged (this is the spec's one behavior change). Pinned by `relayedLineSurvivesItsSenderButADirectOneDoesNot` (Task 1), which also shows the direct path loses it.
- **The "write system log to file" setting.** Expected: `MainWindow`'s startup `enable_log_write_to_file(true)` still produces a `log_fastecu_*.txt` file. Pinned by `enablingFileLoggingWritesASyslogFile` (Task 1) and `windowEnablesFileLoggingThroughTheChannel` (Task 2).
- **Losing the network connection mid-session.** Expected: the "Network connection lost" dialog still fires because `RemoteUtility::stateChanged` reaches `MainWindow`. Pinned by `remoteStateChangesReachThePeer` (Task 3) and `peerStateChangesReachTheWindow` (Task 4).
- **Startup with and without `--host`.** Expected: a direct session never waits; a remote session blocks in `RemoteUtility::waitForSource`; mirroring with no peer neither blocks nor crashes. Pinned by `waitRequestIsWiredToTheRemoteUtility` and `mirroringWithoutAPeerReturnsPromptly` (Task 3) and `directSessionStartupNeverRequestsTheRemoteWait` (Task 4). The remote-session wait itself cannot run headless (it loops until a peer answers) and is covered only by the wiring check.

---

## File Structure

| File | Change | Task |
|---|---|---|
| `src/ui/desktop/channels/BUILD.bazel` | Create: package, `log_channel`, later `remote_peer` | 1, 3 |
| `src/ui/desktop/channels/log_channel.h` | Create | 1 |
| `src/ui/desktop/channels/remote_peer.h` | Create | 3 |
| `src/ui/desktop/main_window_services.h` | Add `log`, `remote`; later drop `syslogger`, `remote_utility` | 1–4 |
| `apps/desktop/desktop_composition.{h,cpp}` | Own and wire the channels; `friend class DesktopCompositionTest` | 1, 3, 2, 4 |
| `apps/desktop/desktop_composition_test.cpp` | New wiring cases | 1–4 |
| `apps/desktop/BUILD.bazel` | Deps on the channels | 1, 3 |
| `src/ui/desktop/mainwindow.{h,cpp}`, `src/ui/desktop/menu_actions.cpp` | Move off `SystemLogger`/`RemoteUtility` | 2, 4 |
| `src/ui/desktop/mainwindow_test.cpp` | Fixture owns the channels; new cases | 1–4 |
| `src/ui/desktop/BUILD.bazel` | Swap platform deps for channel deps | 1–4 |
| `src/platform/desktop/common/logging/BUILD.bazel` | Drop GRANDFATHERED; target-level UI visibility for `logging_runtime`, `logging_adapters` | 2 |
| `src/platform/desktop/common/remote_utility/BUILD.bazel` | Drop GRANDFATHERED | 4 |
| `docs/modularization-plan.md`, `docs/tech-debt.md`, `docs/design-notes.md`, `CLAUDE.md` | Close-out | 5 |
| this plan and the spec | Delete | 5 |

---

### Task 1: `LogChannel`, owned and wired by the composition (6j-1)

**Files:**
- Create: `src/ui/desktop/channels/BUILD.bazel`, `src/ui/desktop/channels/log_channel.h`
- Modify: `src/ui/desktop/main_window_services.h`, `apps/desktop/desktop_composition.h`, `apps/desktop/desktop_composition.cpp`, `apps/desktop/BUILD.bazel`, `apps/desktop/desktop_composition_test.cpp`, `src/ui/desktop/mainwindow_test.cpp`, `src/ui/desktop/BUILD.bazel`

**Interfaces:**
- Produces: `fastecu::ui::LogChannel` in `src/ui/desktop/channels/log_channel.h`, label `//src/ui/desktop/channels:log_channel`, with signals `LOG_E/LOG_W/LOG_I/LOG_D(QString message, bool timestamp, bool linefeed)`, `enable_log_write_to_file(bool enable)`, `log_window_message(QString message)`.
- Produces: `MainWindowServices::log` (`fastecu::ui::LogChannel&`), declared right after `syslogger`. `syslogger` stays until Task 2.
- Produces: `DesktopComposition::log_channel_` (value member) and `friend class DesktopCompositionTest;` in `DesktopComposition`.

- [ ] **Step 1: Create the branch on top of the spec branch**

```bash
git switch docs/step6j-ui-channels
git switch -c refactor/step6j-1-log-channel
```

- [ ] **Step 2: Create the channel package**

`src/ui/desktop/channels/BUILD.bazel`:

```starlark
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "qt_cc_library")

# Signal-only endpoints through which the UI reaches desktop services it does
# not name. apps/desktop's composition root connects them to the platform
# objects; the UI connects to them. They hold no logic.
package(default_visibility = [
    "//apps/desktop:__pkg__",
    "//src/ui/desktop:__subpackages__",
])

qt_cc_library(
    name = "log_channel",
    srcs = [],
    hdrs = ["log_channel.h"],
    copts = COMMON_COPTS,
    deps = QT_DEPS,
)
```

`src/ui/desktop/channels/log_channel.h`:

```cpp
#pragma once

#include <QObject>
#include <QString>

namespace fastecu::ui
{

// The UI's system-log endpoint. DesktopComposition connects it to the desktop
// system logger; UI objects connect their own LOG_* signals to the signals of
// the same name here. The names are part of the contract: the logger reads a
// line's level from the name of the signal that delivered it. Relaying
// through this long-lived object also keeps a line whose original sender is
// destroyed before the logger's thread delivers it.
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

} // namespace fastecu::ui
```

Run: `bazel build --config=release //src/ui/desktop/channels:log_channel`
Expected: builds. If `qt_cc_library` rejects an empty `srcs`, add `log_channel.cpp` containing only `#include "src/ui/desktop/channels/log_channel.h"` and set `srcs = ["log_channel.cpp"]`.

- [ ] **Step 3: Write the failing composition tests**

In `apps/desktop/desktop_composition_test.cpp`, add these includes after the existing ones:

```cpp
#include <QDir>
#include <QFile>
#include <QSemaphore>
#include <QSignalSpy>

#include <algorithm>
#include <memory>

#include "src/platform/desktop/common/logging/systemlogger.h"
#include "src/ui/desktop/channels/log_channel.h"
```

Above `class DesktopCompositionTest`, add:

```cpp
namespace
{

using fastecu::ui::LogChannel;

// Holds the syslog thread inside one queued call until release(), so a test
// can emit and destroy senders before the logger sees their lines.
class SyslogGate
{
  public:
    explicit SyslogGate(SystemLogger& logger)
    {
        QMetaObject::invokeMethod(
            &logger,
            [this]
            {
                entered_.release();
                open_.acquire();
            },
            Qt::QueuedConnection);
        entered_.acquire();
    }
    ~SyslogGate()
    {
        release();
    }
    SyslogGate(const SyslogGate&) = delete;
    SyslogGate& operator=(const SyslogGate&) = delete;

    void release()
    {
        if (!released_)
        {
            released_ = true;
            open_.release();
        }
    }

  private:
    QSemaphore entered_;
    QSemaphore open_;
    bool released_ = false;
};

bool has_line_ending_with(const QSignalSpy& spy, const QString& suffix)
{
    return std::ranges::any_of(spy, [&](const QList<QVariant>& arguments)
                               { return arguments.at(0).toString().endsWith(suffix); });
}

bool has_line_containing(const QSignalSpy& spy, const QString& text)
{
    return std::ranges::any_of(spy, [&](const QList<QVariant>& arguments)
                               { return arguments.at(0).toString().contains(text); });
}

} // namespace
```

Add these private slots to `DesktopCompositionTest`:

```cpp
    void channelLevelsReachTheLogWindowWithTheirPrefix()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        LogChannel& log = composition.services().log;
        QSignalSpy window{&log, &LogChannel::log_window_message};

        emit log.LOG_E("error line", true, false);
        emit log.LOG_W("warning line", true, false);
        emit log.LOG_I("info line", true, false);

        // Match by content: the logger's own "SystemLogger started..." line
        // can reach the window too.
        QTRY_VERIFY(has_line_ending_with(window, "(II) info line"));
        QVERIFY(has_line_ending_with(window, "(EE) error line"));
        QVERIFY(has_line_ending_with(window, "(WW) warning line"));
    }

    void debugLinesStayOutOfTheLogWindow()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        LogChannel& log = composition.services().log;
        QSignalSpy window{&log, &LogChannel::log_window_message};

        emit log.LOG_D("debug line", true, false);
        emit log.LOG_I("sentinel", false, false);

        QTRY_VERIFY(has_line_ending_with(window, "sentinel"));
        QVERIFY(!has_line_containing(window, "debug line"));
    }

    // A dialog that logs and is destroyed before the syslog thread delivers
    // its line: through the channel the line survives; connected straight to
    // the logger, as UI code did before step 6j, it is dropped.
    void relayedLineSurvivesItsSenderButADirectOneDoesNot()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        LogChannel& log = composition.services().log;
        QSignalSpy window{&log, &LogChannel::log_window_message};

        {
            SyslogGate gate{*composition.syslogger_};
            auto relayed = std::make_unique<LogChannel>();
            QObject::connect(relayed.get(), &LogChannel::LOG_I, &log, &LogChannel::LOG_I);
            auto direct = std::make_unique<LogChannel>();
            QObject::connect(direct.get(), &LogChannel::LOG_I, composition.syslogger_.get(),
                             &SystemLogger::log_messages);

            emit relayed->LOG_I("relayed line", false, false);
            emit direct->LOG_I("direct line", false, false);
            relayed.reset();
            direct.reset();
        }
        emit log.LOG_I("sentinel", false, false);

        QTRY_VERIFY(has_line_ending_with(window, "sentinel"));
        QVERIFY(has_line_ending_with(window, "relayed line"));
        QVERIFY(!has_line_containing(window, "direct line"));
    }

    void enablingFileLoggingWritesASyslogFile()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        const QString syslog_dir = composition.services().file_actions.ConfigValuesStruct.syslog_files_directory;
        QVERIFY(!syslog_dir.isEmpty());
        QVERIFY(QDir().mkpath(syslog_dir));
        LogChannel& log = composition.services().log;
        QSignalSpy window{&log, &LogChannel::log_window_message};

        emit log.enable_log_write_to_file(true);
        emit log.LOG_I("to file", false, true);
        // log_messages signals the window before it writes the file; the
        // sentinel's window line proves the earlier write has finished.
        emit log.LOG_I("sentinel", false, false);

        QTRY_VERIFY(has_line_ending_with(window, "sentinel"));
        const QStringList files = QDir(syslog_dir).entryList({"log_fastecu_*.txt"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QFile file{QDir(syslog_dir).filePath(files.first())};
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("to file"));
    }
```

Extend `servicesReferToTheCompositionsOwnObjects` with, after the `syslogger` comparison:

```cpp
        QCOMPARE(&first.log, &second.log);
```

In `apps/desktop/BUILD.bazel`, add to `desktop_composition_test`'s `deps`:

```starlark
        "//src/platform/desktop/common/logging",
        "//src/ui/desktop/channels:log_channel",
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `bazel test --config=release //apps/desktop:desktop_composition_test`
Expected: FAIL to compile — `no member named 'log' in 'MainWindowServices'` and `'syslogger_' is a private member`.

- [ ] **Step 5: Add `log` to `MainWindowServices`**

In `src/ui/desktop/main_window_services.h`, add to the forward declarations:

```cpp
namespace fastecu::ui
{
class LogChannel;
}
```

and in the struct, right after `SystemLogger& syslogger;`:

```cpp
    fastecu::ui::LogChannel& log;
```

- [ ] **Step 6: Own and wire the channel in the composition**

In `apps/desktop/desktop_composition.h`, add `#include "src/ui/desktop/channels/log_channel.h"` after the `main_window_services.h` include; add as the first line inside `class DesktopComposition {`:

```cpp
    friend class DesktopCompositionTest;
```

and after `FileActions file_actions_;`:

```cpp
    fastecu::ui::LogChannel log_channel_;
```

In `apps/desktop/desktop_composition.cpp`, directly after `syslogger_->moveToThread(syslog_thread_.get());`:

```cpp
    // The UI logs through the channel: the logger reads each line's level from
    // the channel's LOG_* signal name, and the channel outlives every sender.
    using fastecu::ui::LogChannel;
    QObject::connect(&log_channel_, &LogChannel::LOG_E, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(&log_channel_, &LogChannel::LOG_W, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(&log_channel_, &LogChannel::LOG_I, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(&log_channel_, &LogChannel::LOG_D, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(&log_channel_, &LogChannel::enable_log_write_to_file, syslogger_.get(),
                     &SystemLogger::enable_log_write_to_file);
    QObject::connect(syslogger_.get(), &SystemLogger::send_message_to_log_window, &log_channel_,
                     &LogChannel::log_window_message);
```

In `services()`, after `.syslogger = *syslogger_,`:

```cpp
        .log = log_channel_,
```

In `apps/desktop/BUILD.bazel`, add `"//src/ui/desktop/channels:log_channel",` to `composition`'s `deps`.

- [ ] **Step 7: Give the MainWindow fixture a channel**

In `src/ui/desktop/mainwindow_test.cpp`, add `#include "src/ui/desktop/channels/log_channel.h"` after the `remote_utility.h` include. In `TestServices`, add the member after `std::unique_ptr<SystemLogger> syslogger;`:

```cpp
    fastecu::ui::LogChannel log_channel;
```

and in `services()` after `.syslogger = *syslogger,`:

```cpp
            .log = log_channel,
```

In `src/ui/desktop/BUILD.bazel`, add `"//src/ui/desktop/channels:log_channel",` to `test_mainwindow`'s `deps`.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `bazel test --config=release //apps/desktop:desktop_composition_test //src/ui/desktop:test_mainwindow`
Expected: PASS.

- [ ] **Step 9: Mutation check for the level wiring**

Temporarily swap `&LogChannel::LOG_E` and `&LogChannel::LOG_W` in the first two composition `connect` lines. Run `bazel test --config=release //apps/desktop:desktop_composition_test`; expected FAIL in `channelLevelsReachTheLogWindowWithTheirPrefix`. Then temporarily delete the `enable_log_write_to_file` connect; expected FAIL in `enablingFileLoggingWritesASyslogFile`. Restore both (`git diff apps/desktop/desktop_composition.cpp` shows only the Step 6 additions) and rerun to PASS.

- [ ] **Step 10: Commit**

```bash
git add src/ui/desktop/channels apps/desktop src/ui/desktop/main_window_services.h src/ui/desktop/mainwindow_test.cpp src/ui/desktop/BUILD.bazel
git commit -m "refactor(desktop): add LogChannel and wire it to the system logger (step 6j-1)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ"
```

---

### Task 2: Move the UI onto `LogChannel` and drop the logging edge (6j-1)

**Files:**
- Modify: `src/ui/desktop/mainwindow.h`, `src/ui/desktop/mainwindow.cpp`, `src/ui/desktop/menu_actions.cpp`, `src/ui/desktop/main_window_services.h`, `src/ui/desktop/mainwindow_test.cpp`, `src/ui/desktop/BUILD.bazel`, `apps/desktop/desktop_composition.h`, `apps/desktop/desktop_composition.cpp`, `apps/desktop/desktop_composition_test.cpp`, `src/platform/desktop/common/logging/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::ui::LogChannel`, `MainWindowServices::log` (Task 1).
- Produces: `MainWindow::log_channel` (`fastecu::ui::LogChannel *`, replacing `SystemLogger *syslogger`). `MainWindowServices::syslogger` no longer exists.

- [ ] **Step 1: Write the failing MainWindow tests**

In `src/ui/desktop/mainwindow_test.cpp`, add after `directSessionStartupNeverWaitsForARemoteSource`:

```cpp
    void windowLogLinesReachTheLogChannel()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        MainWindow window{services.services()};
        constructor_driver.stop();
        QSignalSpy lines{&services.log_channel, &fastecu::ui::LogChannel::LOG_I};

        emit window.LOG_I("probe line", true, false);

        QCOMPARE(lines.count(), 1);
        QCOMPARE(lines.at(0).at(0).toString(), QString("probe line"));
        QCOMPARE(lines.at(0).at(1).toBool(), true);
        QCOMPARE(lines.at(0).at(2).toBool(), false);
    }

    void windowEnablesFileLoggingThroughTheChannel()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QSignalSpy enables{&services.log_channel, &fastecu::ui::LogChannel::enable_log_write_to_file};
        MainWindow window{services.services()};
        constructor_driver.stop();

        QVERIFY(std::ranges::any_of(enables, [](const QList<QVariant>& arguments) { return arguments.at(0).toBool(); }));
    }
```

- [ ] **Step 2: Run them to verify they fail**

Run: `bazel test --config=release //src/ui/desktop:test_mainwindow --test_arg=windowLogLinesReachTheLogChannel --test_arg=windowEnablesFileLoggingThroughTheChannel`
Expected: FAIL — `lines.count()` is 0 and no `true` enable is seen: `MainWindow` still connects to `SystemLogger`.

- [ ] **Step 3: Switch `mainwindow.h` to the channel**

In `src/ui/desktop/mainwindow.h`:
- Delete `#include "src/platform/desktop/common/logging/systemlogger.h"` and the blank line after it.
- Add `#include "src/ui/desktop/channels/log_channel.h"` after `#include "src/ui/desktop/hexedit/hexedit.h"`.
- Replace `SystemLogger *syslogger;` with `fastecu::ui::LogChannel *log_channel = nullptr;`.

- [ ] **Step 4: Rewrite the logger connections**

Run from the repo root:

```bash
perl -0pi -e 's/(&[\w:]+::LOG_([EWID])),(\s*)syslogger,(\s*)&SystemLogger::log_messages/$1,$3log_channel,$4&fastecu::ui::LogChannel::LOG_$2/g' src/ui/desktop/mainwindow.cpp src/ui/desktop/menu_actions.cpp
grep -n "syslogger\|SystemLogger" src/ui/desktop/mainwindow.cpp src/ui/desktop/menu_actions.cpp
```

Expected grep output: only the constructor block (`syslogger = &services_.syslogger;`, the `enable_log_write_to_file` connect, and the `send_message_to_log_window` connect). Replace that block in `mainwindow.cpp` (starting `syslogger = &services_.syslogger;`) with:

```cpp
    log_channel = &services_.log;
    using fastecu::ui::LogChannel;
    QObject::connect(this, &MainWindow::LOG_E, log_channel, &LogChannel::LOG_E);
    QObject::connect(this, &MainWindow::LOG_W, log_channel, &LogChannel::LOG_W);
    QObject::connect(this, &MainWindow::LOG_I, log_channel, &LogChannel::LOG_I);
    QObject::connect(this, &MainWindow::LOG_D, log_channel, &LogChannel::LOG_D);
    QObject::connect(this, &MainWindow::enable_log_write_to_file, log_channel,
                     &LogChannel::enable_log_write_to_file);
    QObject::connect(log_channel, &LogChannel::log_window_message, this, &MainWindow::send_message_to_log_window);
```

In the flash controller relay comment in `mainwindow.cpp` ("Relay through MainWindow's own LOG_* signals: the syslogger runs on its own thread, …"), replace the comment with:

```cpp
        // Relay through MainWindow's own LOG_* signals, like every UI logger;
        // see LogChannel for why lines go through a long-lived sender.
```

Rerun the grep; expected: no output.

- [ ] **Step 5: Remove `syslogger` from the services and composition**

- `src/ui/desktop/main_window_services.h`: delete `class SystemLogger;` and `SystemLogger& syslogger;`.
- `apps/desktop/desktop_composition.cpp`: delete `.syslogger = *syslogger_,` from `services()`.
- `apps/desktop/desktop_composition_test.cpp`: delete `QCOMPARE(&first.syslogger, &second.syslogger);`.
- `src/ui/desktop/mainwindow_test.cpp`: delete `#include "src/platform/desktop/common/logging/systemlogger.h"`, the `syslogger = std::make_unique<SystemLogger>(...)` statement in the `TestServices` constructor, `.syslogger = *syslogger,`, and the `std::unique_ptr<SystemLogger> syslogger;` member. Replace the fixture comment "The services DesktopComposition builds in the real app, minus the syslog thread: the logger lives on the test thread, which is enough for a receiver." with "The services DesktopComposition builds in the real app. The channels are left unwired: tests spy on them."
- In the `handledDensoTcuReadChoices…` test, replace the comment "The TCU log lines must come from MainWindow, which outlives the queued delivery to the syslogger's thread; a short-lived sender's queued lines are dropped once it is destroyed." with "The TCU log lines are relayed through MainWindow's own LOG_* signals."

- [ ] **Step 6: Split the logging package's visibility**

In `src/platform/desktop/common/logging/BUILD.bazel`, replace the `package(...)` call with:

```starlark
package(default_visibility = [
    "//apps/desktop:__pkg__",
    "//src/platform:__subpackages__",
    "//tests:__pkg__",
])

# UI-facing adapters, like connection:adapter_connection: MainWindow holds the
# LoggingEngine and builds its snapshots and values through these adapters.
UI_FACING_VISIBILITY = [
    "//apps/desktop:__pkg__",
    "//src/platform:__subpackages__",
    "//src/ui/desktop:__pkg__",
    "//tests:__pkg__",
]
```

Add `visibility = UI_FACING_VISIBILITY,` to the `logging_runtime` and `logging_adapters` rules (after `normal_hdrs`, before `deps`, as buildifier orders it).

In `src/ui/desktop/BUILD.bazel`, delete `"//src/platform/desktop/common/logging",` from both `desktop`'s and `test_mainwindow`'s `deps`, and add `"//src/ui/desktop/channels:log_channel",` to `desktop`'s `deps`.

- [ ] **Step 7: Build and run everything**

Run: `bazel test --config=release //...`
Expected: PASS. If `mainwindow.cpp`, `menu_actions.cpp`, or another `:desktop` source fails with an unknown Qt name (`QFile`, `QTime`, `QMetaMethod`, `QDebug`, `QApplication` came in through `systemlogger.h`), add the matching `#include <QName>` to the file that uses it — never re-add the logging dep.

- [ ] **Step 8: Prove the edge is closed**

Temporarily add `"//src/platform/desktop/common/logging",` back to `//src/ui/desktop:desktop`'s `deps`. Run: `bazel build --config=release //src/ui/desktop:desktop 2>&1 | grep -m1 "not visible"`
Expected: one line reporting `target '//src/platform/desktop/common/logging:logging' is not visible from target '//src/ui/desktop:desktop'`. Revert the line and save the error text for the PR description.

- [ ] **Step 9: Lint and commit**

```bash
prek run --all-files
bazel run //:clang_tidy_report_changed
git add -A src/ui/desktop apps/desktop src/platform/desktop/common/logging/BUILD.bazel
git commit -m "refactor(ui): log through LogChannel and drop the GRANDFATHERED logging edge (step 6j-1)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ"
```

---

### Task 3: `RemotePeer`, owned and wired by the composition (6j-2)

**Files:**
- Create: `src/ui/desktop/channels/remote_peer.h`
- Modify: `src/ui/desktop/channels/BUILD.bazel`, `src/ui/desktop/main_window_services.h`, `apps/desktop/desktop_composition.h`, `apps/desktop/desktop_composition.cpp`, `apps/desktop/BUILD.bazel`, `apps/desktop/desktop_composition_test.cpp`, `src/ui/desktop/mainwindow_test.cpp`, `src/ui/desktop/BUILD.bazel`

**Interfaces:**
- Consumes: `DesktopComposition`'s `friend class DesktopCompositionTest` (Task 1).
- Produces: `fastecu::ui::RemotePeer` in `src/ui/desktop/channels/remote_peer.h`, label `//src/ui/desktop/channels:remote_peer`: public `void wait_for_source()`; signals `wait_requested()`, `log_window_message(QString message)`, `progress(int value)`, `stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State old_state)`.
- Produces: `MainWindowServices::remote` (`fastecu::ui::RemotePeer&`), declared right after `remote_utility`, which stays until Task 4. `DesktopComposition::remote_peer_` (value member).

- [ ] **Step 1: Create the branch**

```bash
git switch refactor/step6j-1-log-channel
git switch -c refactor/step6j-2-remote-peer
```

- [ ] **Step 2: Create the channel**

Append to `src/ui/desktop/channels/BUILD.bazel`:

```starlark
qt_cc_library(
    name = "remote_peer",
    srcs = [],
    hdrs = ["remote_peer.h"],
    copts = COMMON_COPTS,
    deps = QT_DEPS,
)
```

(Use the same `.cpp` fallback as Task 1 Step 2 if `log_channel` needed it.)

`src/ui/desktop/channels/remote_peer.h`:

```cpp
#pragma once

#include <QObject>
#include <QRemoteObjectReplica>
#include <QString>

namespace fastecu::ui
{

// The remote utility peer as MainWindow sees it. DesktopComposition connects
// it to the remote utility replica: the wait, the state changes, and the
// log/progress mirror, which it drops while the replica is not valid.
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
    void log_window_message(QString message);
    void progress(int value);
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State old_state);
};

} // namespace fastecu::ui
```

Run: `bazel build --config=release //src/ui/desktop/channels:remote_peer`
Expected: builds.

- [ ] **Step 3: Write the failing composition tests**

In `apps/desktop/desktop_composition_test.cpp`, add includes:

```cpp
#include <QMetaMethod>

#include "src/platform/desktop/common/remote_utility/remote_utility.h"
#include "src/ui/desktop/channels/remote_peer.h"
```

and `using fastecu::ui::RemotePeer;` next to `using fastecu::ui::LogChannel;`. Add private slots:

```cpp
    void waitRequestIsWiredToTheRemoteUtility()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        // Running the wait needs a peer; it loops until one answers.
        QVERIFY(composition.services().remote.isSignalConnected(QMetaMethod::fromSignal(&RemotePeer::wait_requested)));
    }

    void remoteStateChangesReachThePeer()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        QSignalSpy changes{&composition.services().remote, &RemotePeer::stateChanged};

        emit composition.remote_utility_->stateChanged(QRemoteObjectReplica::Suspect, QRemoteObjectReplica::Valid);

        QCOMPARE(changes.count(), 1);
        QCOMPARE(changes.at(0).at(0).value<QRemoteObjectReplica::State>(), QRemoteObjectReplica::Suspect);
        QCOMPARE(changes.at(0).at(1).value<QRemoteObjectReplica::State>(), QRemoteObjectReplica::Valid);
    }

    // No --host: the replica never becomes valid, so the mirror drops both.
    void mirroringWithoutAPeerReturnsPromptly()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        RemotePeer& remote = composition.services().remote;
        QVERIFY(!composition.remote_utility_->isValid());

        QElapsedTimer elapsed;
        elapsed.start();
        emit remote.log_window_message("mirrored line");
        emit remote.progress(42);
        QVERIFY2(elapsed.elapsed() < 1000, "mirroring without a peer blocked");
    }
```

Extend `servicesReferToTheCompositionsOwnObjects` with, after the `remote_utility` comparison:

```cpp
        QCOMPARE(&first.remote, &second.remote);
```

In `apps/desktop/BUILD.bazel`, add to `desktop_composition_test`'s `deps`:

```starlark
        "//src/platform/desktop/common/remote_utility",
        "//src/ui/desktop/channels:remote_peer",
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `bazel test --config=release //apps/desktop:desktop_composition_test`
Expected: FAIL to compile — `no member named 'remote' in 'MainWindowServices'`.

- [ ] **Step 5: Add `remote` to `MainWindowServices`**

In `src/ui/desktop/main_window_services.h`, extend the `fastecu::ui` forward-declaration block with `class RemotePeer;`, and add right after `RemoteUtility& remote_utility;`:

```cpp
    fastecu::ui::RemotePeer& remote;
```

- [ ] **Step 6: Own and wire the peer in the composition**

In `apps/desktop/desktop_composition.h`, add `#include "src/ui/desktop/channels/remote_peer.h"` after the `log_channel.h` include, and after `fastecu::ui::LogChannel log_channel_;`:

```cpp
    fastecu::ui::RemotePeer remote_peer_;
```

In `apps/desktop/desktop_composition.cpp`, directly after `remote_utility_ = std::make_unique<RemoteUtility>(...);`:

```cpp
    // The UI reaches the remote utility only through the peer channel. The
    // mirror is dropped while the replica is not valid, as MainWindow did.
    using fastecu::ui::RemotePeer;
    QObject::connect(&remote_peer_, &RemotePeer::wait_requested, remote_utility_.get(), &RemoteUtility::waitForSource,
                     Qt::DirectConnection);
    QObject::connect(&remote_peer_, &RemotePeer::log_window_message, remote_utility_.get(),
                     [utility = remote_utility_.get()](const QString& message)
                     {
                         if (utility->isValid())
                         {
                             utility->send_log_window_message(message);
                         }
                     });
    QObject::connect(&remote_peer_, &RemotePeer::progress, remote_utility_.get(),
                     [utility = remote_utility_.get()](int value)
                     {
                         if (utility->isValid())
                         {
                             utility->set_progressbar_value(value);
                         }
                     });
    QObject::connect(remote_utility_.get(), &RemoteUtility::stateChanged, &remote_peer_, &RemotePeer::stateChanged);
```

In `services()`, after `.remote_utility = *remote_utility_,`:

```cpp
        .remote = remote_peer_,
```

In `apps/desktop/BUILD.bazel`, add `"//src/ui/desktop/channels:remote_peer",` to `composition`'s `deps`.

- [ ] **Step 7: Give the MainWindow fixture a peer**

In `src/ui/desktop/mainwindow_test.cpp`, add `#include "src/ui/desktop/channels/remote_peer.h"` after the `log_channel.h` include. In `TestServices`, add after `RemoteUtility remote_utility{"", ""};`:

```cpp
    fastecu::ui::RemotePeer remote_peer;
```

and in `services()` after `.remote_utility = remote_utility,`:

```cpp
            .remote = remote_peer,
```

In `src/ui/desktop/BUILD.bazel`, add `"//src/ui/desktop/channels:remote_peer",` to `test_mainwindow`'s `deps`.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `bazel test --config=release //apps/desktop:desktop_composition_test //src/ui/desktop:test_mainwindow`
Expected: PASS.

- [ ] **Step 9: Mutation check for the state forwarding**

Temporarily delete the `RemoteUtility::stateChanged` connect. Run `bazel test --config=release //apps/desktop:desktop_composition_test`; expected FAIL in `remoteStateChangesReachThePeer`. Temporarily delete the `wait_requested` connect instead; expected FAIL in `waitRequestIsWiredToTheRemoteUtility`. Restore and rerun to PASS.

- [ ] **Step 10: Commit**

```bash
git add src/ui/desktop/channels apps/desktop src/ui/desktop/main_window_services.h src/ui/desktop/mainwindow_test.cpp src/ui/desktop/BUILD.bazel
git commit -m "refactor(desktop): add RemotePeer and wire it to the remote utility (step 6j-2)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ"
```

---

### Task 4: Move the UI onto `RemotePeer` and drop the remote-utility edge (6j-2)

**Files:**
- Modify: `src/ui/desktop/mainwindow.h`, `src/ui/desktop/mainwindow.cpp`, `src/ui/desktop/main_window_services.h`, `src/ui/desktop/mainwindow_test.cpp`, `src/ui/desktop/BUILD.bazel`, `apps/desktop/desktop_composition.cpp`, `apps/desktop/desktop_composition_test.cpp`, `src/platform/desktop/common/remote_utility/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::ui::RemotePeer`, `MainWindowServices::remote` (Task 3).
- Produces: `MainWindow::remote_peer` (`fastecu::ui::RemotePeer *`, replacing `RemoteUtility *remote_utility`). `MainWindowServices::remote_utility` no longer exists.

- [ ] **Step 1: Write the failing MainWindow tests**

In `src/ui/desktop/mainwindow_test.cpp`, add after `windowEnablesFileLoggingThroughTheChannel`:

```cpp
    void directSessionStartupNeverRequestsTheRemoteWait()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QSignalSpy waits{&services.remote_peer, &fastecu::ui::RemotePeer::wait_requested};
        MainWindow window{services.services()};
        constructor_driver.stop();

        QCOMPARE(waits.count(), 0);
    }

    void externalLoggerMirrorsToTheRemotePeer()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        MainWindow window{services.services()};
        constructor_driver.stop();
        QSignalSpy lines{&services.remote_peer, &fastecu::ui::RemotePeer::log_window_message};
        QSignalSpy progress{&services.remote_peer, &fastecu::ui::RemotePeer::progress};

        // Private slots: call by name so the Windows link needs no mangled
        // private symbol (see startEcuOperations).
        QVERIFY(QMetaObject::invokeMethod(&window, "external_logger", Qt::DirectConnection,
                                          Q_ARG(QString, QString("mirrored line"))));
        QVERIFY(QMetaObject::invokeMethod(&window, "external_logger_set_progressbar_value", Qt::DirectConnection,
                                          Q_ARG(int, 42)));

        QCOMPARE(lines.count(), 1);
        QCOMPARE(lines.at(0).at(0).toString(), QString("mirrored line"));
        QCOMPARE(progress.count(), 1);
        QCOMPARE(progress.at(0).at(0).toInt(), 42);
    }

    void peerStateChangesReachTheWindow()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        MainWindow window{services.services()};
        constructor_driver.stop();
        QSignalSpy debug_lines{&window, &MainWindow::LOG_D};

        emit services.remote_peer.stateChanged(QRemoteObjectReplica::Valid, QRemoteObjectReplica::Default);

        QVERIFY(std::ranges::any_of(debug_lines, [](const QList<QVariant>& arguments)
                                    { return arguments.at(0).toString() == "Network connection established"; }));
    }
```

(`peerStateChangesReachTheWindow` uses the `Valid` branch of `network_state_changed`, which only logs; the lost-connection branch opens a modal dialog.)

- [ ] **Step 2: Run them to verify they fail**

Run: `bazel test --config=release //src/ui/desktop:test_mainwindow --test_arg=externalLoggerMirrorsToTheRemotePeer --test_arg=peerStateChangesReachTheWindow --test_arg=directSessionStartupNeverRequestsTheRemoteWait`
Expected: `externalLoggerMirrorsToTheRemotePeer` and `peerStateChangesReachTheWindow` FAIL (counts 0; no "established" line). `directSessionStartupNeverRequestsTheRemoteWait` passes already; it guards Step 3.

- [ ] **Step 3: Switch `MainWindow` to the peer**

In `src/ui/desktop/mainwindow.h`:
- Delete `#include "src/platform/desktop/common/remote_utility/remote_utility.h"` and the blank line after it.
- Add `#include "src/ui/desktop/channels/remote_peer.h"` after the `log_channel.h` include. (`remote_peer.h` brings `<QRemoteObjectReplica>` for `network_state_changed`; also add `#include <QRemoteObjectReplica>` directly next to the other Qt includes, since `mainwindow.h` names the type itself.)
- Replace `RemoteUtility *remote_utility = nullptr;` with `fastecu::ui::RemotePeer *remote_peer = nullptr;`.

In `src/ui/desktop/mainwindow.cpp`:
- `remote_utility = &services_.remote_utility;` → `remote_peer = &services_.remote;`
- `remote_utility->waitForSource();` → `remote_peer->wait_for_source();`
- `connect(remote_utility, &RemoteUtility::stateChanged, this, &MainWindow::network_state_changed,` → `connect(remote_peer, &fastecu::ui::RemotePeer::stateChanged, this, &MainWindow::network_state_changed,` (keep `Qt::DirectConnection`).
- In `external_logger`, replace the `if (remote_utility->isValid()) { … }` block with `emit remote_peer->log_window_message(message);`.
- In `external_logger_set_progressbar_value`, replace the `if (remote_utility->isValid()) { … }` block with `emit remote_peer->progress(value);`.

Run: `grep -n "remote_utility\|RemoteUtility" src/ui/desktop/*.cpp src/ui/desktop/*.h | grep -v _test.cpp`
Expected: no output.

- [ ] **Step 4: Remove `remote_utility` from the services and composition**

- `src/ui/desktop/main_window_services.h`: delete `class RemoteUtility;` and `RemoteUtility& remote_utility;`.
- `apps/desktop/desktop_composition.cpp`: delete `.remote_utility = *remote_utility_,`.
- `apps/desktop/desktop_composition_test.cpp`: delete `QCOMPARE(&first.remote_utility, &second.remote_utility);`.
- `src/ui/desktop/mainwindow_test.cpp`: delete `#include "src/platform/desktop/common/remote_utility/remote_utility.h"`, `.remote_utility = remote_utility,`, and `RemoteUtility remote_utility{"", ""};`.

- [ ] **Step 5: Drop the GRANDFATHERED entry and the UI deps**

In `src/platform/desktop/common/remote_utility/BUILD.bazel`, delete the four-line `# GRANDFATHERED …` comment and `"//src/ui/desktop:__pkg__",` from `default_visibility`.

In `src/ui/desktop/BUILD.bazel`, delete `"//src/platform/desktop/common/remote_utility",` from both `desktop`'s and `test_mainwindow`'s `deps`, and add `"//src/ui/desktop/channels:remote_peer",` to `desktop`'s `deps`.

- [ ] **Step 6: Build and run everything**

Run: `bazel test --config=release //...`
Expected: PASS. A `:desktop` source failing on a Qt name that came through `remote_utility.h` (`QTimer`, `QWebSocket`, `QRemoteObjectNode`) gets the matching `#include` in that file — never the dep back.

Run: `grep -rn GRANDFATHERED --include=BUILD.bazel --include='*.bzl' src apps bazel tests BUILD.bazel`
Expected: no output.

- [ ] **Step 7: Prove the edge is closed**

Temporarily add `"//src/platform/desktop/common/remote_utility",` to `//src/ui/desktop:desktop`'s `deps`. Run: `bazel build --config=release //src/ui/desktop:desktop 2>&1 | grep -m1 "not visible"`
Expected: `target '//src/platform/desktop/common/remote_utility:remote_utility' is not visible from target '//src/ui/desktop:desktop'`. Revert and save the error text for the PR description.

- [ ] **Step 8: Lint and commit**

```bash
prek run --all-files
bazel run //:clang_tidy_report_changed
git add -A src/ui/desktop apps/desktop src/platform/desktop/common/remote_utility/BUILD.bazel
git commit -m "refactor(ui): reach the remote utility through RemotePeer and drop the GRANDFATHERED edge (step 6j-2)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ"
```

---

### Task 5: Close out step 6j in the docs (6j-2)

**Files:**
- Modify: `docs/modularization-plan.md`, `docs/tech-debt.md`, `docs/design-notes.md`, `CLAUDE.md`
- Delete: `docs/superpowers/specs/2026-09-27-step6j-ui-channels-design.md`, `docs/superpowers/plans/2026-09-27-step6j-ui-channels.md`

- [ ] **Step 1: Update the modularization plan**

In `docs/modularization-plan.md`'s Status section, replace the sentences from "Next is **6j**" through "not yet designed." with:

```markdown
**6j (UI channels)** is complete: `MainWindow` reaches the system logger and
the remote utility through two UI-owned channels the composition root wires,
and no GRANDFATHERED visibility entry remains. What is left of step 6 is its
last two bullets below.
```

In step 6's bullet list, insert after the 6i bullet (before "Remove compatibility wrappers, obsolete facades, …"):

```markdown
   - **6j UI channels — complete.** `MainWindowServices` carries two
     signal-only `QObject`s from the new `//src/ui/desktop/channels` package
     instead of `SystemLogger` and `RemoteUtility`: `LogChannel`, whose
     `LOG_*` signals every UI logger relays through, and `RemotePeer`, for
     the startup wait, connection-state changes, and the log/progress mirror.
     `DesktopComposition` connects both to the platform objects and keeps the
     remote mirror's `isValid` check. The two GRANDFATHERED `//src/ui/desktop`
     entries are gone; `logging_runtime` and `logging_adapters` name the UI in
     their own target-level visibility as UI-facing adapters. One behavior
     change: a line logged by a dialog destroyed before the syslog thread
     delivered it used to be dropped and is now logged. Two PRs after the
     spec (6j-1 log channel, 6j-2 remote peer and close-out). See the
     [design notes](design-notes.md#ui-channels).
```

- [ ] **Step 2: Remove the tech-debt entry**

In `docs/tech-debt.md`, delete the whole `### P1: Remove the GRANDFATHERED UI → platform edges (step 6j)` section, from its heading through "- Remove each GRANDFATHERED entry with the code that needed it." and the blank line after it. Then run `grep -n "6j\|GRANDFATHERED" docs/tech-debt.md` and reword any remaining sentence that describes the edges as still present.

- [ ] **Step 3: Add the design-notes section**

In `docs/design-notes.md`, insert before `## Testing`:

```markdown
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
```

Run: `grep -rn "design-notes.md#ui-channels" docs` — expected: the modularization-plan link from Step 1.

- [ ] **Step 4: Update CLAUDE.md**

In `CLAUDE.md`'s "Build-graph guardrails" section, replace the "Ratchet lists only shrink." bullet with:

```markdown
- **Ratchet lists only shrink.** Some guards freeze a list of remaining transitional debt — the `qt_layer` package group in `bazel/qt/BUILD.bazel`. Entries come out as the work lands; **an entry may never go in.** Needing to add one means the change took the legacy path and should be rewritten to take the portable one.
- **A `ui → platform` edge goes to a designed adapter.** A platform target the UI calls directly is written for the UI and names `//src/ui/desktop` in its own target-level `visibility`, never in a package's `default_visibility`. Anything else the UI needs from the platform reaches it through a UI-owned channel or a backend port that the composition root wires.
```

- [ ] **Step 5: Lint and commit the docs**

```bash
prek run --all-files
git add docs/modularization-plan.md docs/tech-debt.md docs/design-notes.md CLAUDE.md
git commit -m "docs: close out step 6j (UI channels)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ"
```

- [ ] **Step 6: Delete the spec and this plan**

```bash
git rm docs/superpowers/specs/2026-09-27-step6j-ui-channels-design.md docs/superpowers/plans/2026-09-27-step6j-ui-channels.md
prek run --all-files
git commit -m "docs: remove the step 6j spec and plan" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ"
```

Expected: `prek` passes — no remaining doc links to the deleted files (`grep -rn "step6j-ui-channels" docs CLAUDE.md` prints nothing).

---

### Task 6: Publish the stack

Only when the user authorizes pushing.

```bash
git push -u origin docs/step6j-ui-channels refactor/step6j-1-log-channel refactor/step6j-2-remote-peer
gh stack init --base master docs/step6j-ui-channels refactor/step6j-1-log-channel refactor/step6j-2-remote-peer
gh stack submit --auto
```

Each PR description names its step (6j-0, 6j-1, 6j-2), states validation (`bazel test --config=release //...`, prek, clang-tidy report), includes the visibility-proof error line (6j-1, 6j-2), and ends with:

```
🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ
```
