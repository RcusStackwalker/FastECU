# Step 6c Desktop Composition Root Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move construction of `MainWindow`'s long-lived services into an owning `DesktopComposition` in `apps/desktop` and inject them through a `MainWindowServices` struct, with no desktop behavior change beyond stopping the leaked syslogger thread.

**Architecture:** `DesktopComposition` (in `apps/desktop`) builds and owns the Qt port adapters, `FileActions`, `SystemLogger` and its thread, the serial facade, `RemoteUtility`, `QtClock`, and `LoggingEngine`, and hands `MainWindow` a struct of references declared in `src/ui/desktop`. The serial facade is reached through a new constructor-only factory target so the frozen `serial_qt_compat` allowlist does not grow. Delivery is four PRs; each is green on its own.

**Tech Stack:** C++23, Qt 6 (Widgets, QtTest), Bazel 9 with Bzlmod, GoogleMock (fake serial backend), prek.

**Spec:** [docs/superpowers/specs/2026-09-26-step6c-composition-root-design.md](../specs/2026-09-26-step6c-composition-root-design.md)

## Global Constraints

- Bazel is the only build graph; every command runs with `--config=release`.
- Never add an entry to `serial_qt_compat`'s `visibility` list or to `FROZEN` in `scripts/check-serial-compat-allowlist.py`; never add a package to `//bazel/qt:qt_layer`.
- Do not change any `serial->` call site, flash dispatch, `FileActions` model types, or logging-protocol registration logic.
- Remote-mode startup (network splash + `waitForSource()`) stays inside `MainWindow`.
- `qt_cc_library`: headers declaring `Q_OBJECT` go in `hdrs`; all other headers go in `normal_hdrs`.
- Tests are co-located with the package they test.
- Backend code is untouched; nothing here enters a portable package.
- Work lands through PRs: one branch per PR, branched from up-to-date `master`. Push and open PRs only when the user authorizes it.
- Commit messages end with the attribution lines from the session's system reminder.
- Before every push: `prek run --all-files`, `bazel test --config=release //...`, and `bazel run //:clang_tidy_report_changed` all pass.

## Review Focus

1. **Restart loop (`RESTART_CODE`)** — a second `DesktopComposition` in the same process must construct and destroy cleanly (the syslogger thread is now stopped per iteration). Pinned by `constructingTwiceInOneProcessSucceeds` in Task 3.
2. **Quit immediately after start** — destroying the composition while `SystemLogger::run()` is still inside its 1-second `delay()` must not hang. Pinned by `destructionRightAfterConstructionDoesNotHang` in Task 3.
3. **Settings save path** — closing Preferences must still write `fastecu.cfg`, now through the shared `FileActions`. Pinned by `test_settings` in Task 2.
4. **Serial log routing** — every `SerialPortActions` log level must still reach the syslogger after the connection moves out of `MainWindow` (string-based connects fail only at runtime). Pinned by `desktop_serial_factory_test` in Task 4.
5. **Remote mode startup (`-s host:port`)** — the facades are now constructed before the startup splash; the network splash and connection must still appear. Not automatable here: manual smoke step in Task 5.

---

## PR 6c-1

### Task 1: Delete the dead `EcuOperations` class

`EcuOperations` (`src/ui/desktop/ecu_operations.{h,cpp}`, 2,315 lines) is compiled but never instantiated. Its only reference is an unassigned pointer member in `get_key_operations_subaru.h` used in one commented-out line. `ecu_operations.ui` stays: `FlashDialog` and `GetKeyOperationsSubaru` use its generated `Ui::EcuOperationsWindow`.

**Files:**
- Delete: `src/ui/desktop/ecu_operations.cpp`, `src/ui/desktop/ecu_operations.h`
- Modify: `src/ui/desktop/BUILD.bazel` (the `srcs` and `hdrs` lists of `:desktop`)
- Modify: `src/ui/desktop/get_key_operations_subaru.h`
- Modify: `src/ui/desktop/get_key_operations_subaru.cpp:42`
- Modify: `docs/tech-debt.md` (two mentions of `ecu_operations.cpp`)

**Interfaces:**
- Consumes: nothing.
- Produces: nothing later tasks rely on.

- [ ] **Step 1: Branch**

```bash
git switch master && git pull --ff-only && git switch -c refactor/step6c-1-delete-ecu-operations
```

- [ ] **Step 2: Confirm the class is unreferenced**

Run: `grep -rn "EcuOperations\b\|ecu_operations\.h" src apps tests | grep -v "^src/ui/desktop/ecu_operations\."`
Expected: only `get_key_operations_subaru.h` (the `#include` and `EcuOperations *ecuOperations{};`) and `mainwindow_test.cpp` (`startEcuOperations`, an unrelated helper name). Anything else: stop and report.

- [ ] **Step 3: Delete the files and their BUILD entries**

```bash
git rm src/ui/desktop/ecu_operations.cpp src/ui/desktop/ecu_operations.h
```

In `src/ui/desktop/BUILD.bazel`, remove the line `"ecu_operations.cpp",` from `:desktop`'s `srcs` and `"ecu_operations.h",` from its `hdrs`. Leave `"ecu_operations.ui",` in `FORMS`.

- [ ] **Step 4: Remove the dead member from `GetKeyOperationsSubaru`**

In `src/ui/desktop/get_key_operations_subaru.h`, delete these two lines:

```cpp
#include "src/ui/desktop/ecu_operations.h"
```

```cpp
    EcuOperations *ecuOperations{};
```

and add `#include <QDialog>` after `#include <QMessageBox>` (the deleted header was the file's route to `QDialog`).

In `src/ui/desktop/get_key_operations_subaru.cpp`, delete the commented-out line `    // ecuOperations->kill_process = true;` in `closeEvent`.

- [ ] **Step 5: Fix the stale doc mentions**

In `docs/tech-debt.md`:
- In "Phase 1", change ``files, `J2534_unix.cpp`, and `ecu_operations.cpp` — the hardware-facing layer`` to ``files and `J2534_unix.cpp` — the hardware-facing layer``.
- In "Phase 3", change ``one pass each on the two worst offenders (`mainwindow.cpp`, `ecu_operations.cpp`).`` to ``one pass on the worst offender (`mainwindow.cpp`; `ecu_operations.cpp`, formerly the other, was dead code and was deleted in step 6c).``

- [ ] **Step 6: Build and test**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //src/ui/... //tests/...`
Expected: build succeeds and all tests PASS. If a compile error names a Qt type in `get_key_operations_subaru.*` that the deleted header used to bring in (for example `QProgressBar`, `QTextEdit`, `QVBoxLayout`), add that one `#include <Q...>` to `get_key_operations_subaru.h` and rebuild. If the error names `FileActions`, `kernelcomms.h`, or `kernelmemorymodels.h`, add the matching `src/backend/definitions/...` include instead.

- [ ] **Step 7: Commit**

```bash
git add -A src/ui/desktop docs/tech-debt.md
git commit -m "refactor(ui): delete the dead EcuOperations class (step 6c-1)"
```

- [ ] **Step 8: Gate and PR (when authorized)**

Run `prek run --all-files`, `bazel test --config=release //...`, `bazel run //:clang_tidy_report_changed`; all must pass. Then push and open a PR titled `refactor(ui): delete the dead EcuOperations class (step 6c-1)`, linking the spec.

---

## PR 6c-2

Branch from `master` after PR 6c-1 merges:

```bash
git switch master && git pull --ff-only && git switch -c refactor/step6c-2-composition-root
```

### Task 2: `Settings` takes the shared `FileActions`

Today `Settings` keeps four port adapters plus a `NullEventSink` only so that `save_config_file()` can build a throwaway `FileActions`. After this task it uses the caller's instance. Side effect (called out in the spec): diagnostics from the save now reach `MainWindow`'s sink instead of being dropped.

**Files:**
- Modify: `src/ui/desktop/settings.h`
- Modify: `src/ui/desktop/settings.cpp:7-13` (constructor) and `:50-58` (`save_config_file`)
- Modify: `src/ui/desktop/menu_actions.cpp:788`
- Create: `src/ui/desktop/settings_test.cpp`
- Modify: `src/ui/desktop/BUILD.bazel` (new `test_settings` target)

**Interfaces:**
- Consumes: `FileActions(fastecu::IFileSystem&, fastecu::IResourceBundle&, fastecu::IFileRepository&, fastecu::IAtomicFileWriter&, fastecu::IEventSink&)`, `FileActions::set_base_dirs(ConfigValuesStructure*, std::string_view)`, `FileActions::check_config_dirs(ConfigValuesStructure*)`, `FileActions::save_config_file(ConfigValuesStructure*)`.
- Produces: `Settings::Settings(FileActions& fileActions, FileActions::ConfigValuesStructure *configValues, QWidget *parent = nullptr)`.

- [ ] **Step 1: Write the failing test**

Create `src/ui/desktop/settings_test.cpp`:

```cpp
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "src/backend/definitions/file_actions.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/ui/desktop/settings.h"

class SettingsTest : public QObject
{
    Q_OBJECT

  private slots:
    void closingSettingsSavesTheConfigThroughTheInjectedFileActions()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QtFileSystem file_system;
        QtResourceBundle resource_bundle;
        QtFileRepository file_repository;
        QtAtomicFileWriter file_writer;
        QtEventSink events;
        FileActions file_actions{file_system, resource_bundle, file_repository, file_writer, events};
        FileActions::ConfigValuesStructure *config = &file_actions.ConfigValuesStruct;
        file_actions.set_base_dirs(config, root.path().toStdString());
        file_actions.check_config_dirs(config);
        // check_config_dirs may seed a default config; remove it so the
        // assertion below can only pass if Settings wrote it.
        QFile::remove(config->config_file);
        QVERIFY(!QFile::exists(config->config_file));

        {
            Settings settings{file_actions, config};
        }

        QVERIFY(QFile::exists(config->config_file));
    }
};

QTEST_MAIN(SettingsTest)
#include "settings_test.moc"
```

Add to `src/ui/desktop/BUILD.bazel` after `test_mainwindow`:

```python
fastecu_qttest(
    name = "test_settings",
    size = "small",
    src = "settings_test.cpp",
    copts = ["-DQT_WIDGETS_LIB"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    # Static for the same reason as test_mainwindow above.
    linkstatic = True,
    deps = [":desktop"],
)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test --config=release //src/ui/desktop:test_settings`
Expected: FAIL to compile: no `Settings` constructor matches `(FileActions&, FileActions::ConfigValuesStructure*)`.

- [ ] **Step 3: Change `Settings`**

In `src/ui/desktop/settings.h`:
- Change the constructor to `explicit Settings(FileActions& fileActions, FileActions::ConfigValuesStructure *configValues, QWidget *parent = nullptr);`
- Replace the comment block and the five members `m_configFileSystem`, `m_configResourceBundle`, `m_configFileRepository`, `m_definitionFileWriter`, `m_fileActionsEvents` with the single member `FileActions& fileActions;`, declared immediately before `FileActions::ConfigValuesStructure *configValues;`.
- Delete the now-unused includes of `qt_atomic_file_writer.h`, `qt_file_repository.h`, `qt_file_system.h`, `qt_resource_bundle.h`, and `src/backend/ports/event_sink.h`.

In `src/ui/desktop/settings.cpp`, change the constructor head to:

```cpp
Settings::Settings(FileActions& fileActions, FileActions::ConfigValuesStructure *configValues, QWidget *parent)
    : QDialog(parent), fileActions(fileActions), ui{std::make_unique<Ui::Settings>()}
```

(`ui` is declared last in the class, so the initializer order matches declaration order.) Replace the body of `save_config_file()` with:

```cpp
int Settings::save_config_file()
{
    qDebug() << "Save config file";
    fileActions.save_config_file(configValues);
    qDebug() << "Config file saved";

    return 0;
}
```

In `src/ui/desktop/menu_actions.cpp:788`, change `Settings settings(configValues);` to `Settings settings(*fileActions, configValues);`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `bazel test --config=release //src/ui/desktop:all`
Expected: `test_settings` and `test_mainwindow` PASS.

- [ ] **Step 5: Commit**

```bash
git add src/ui/desktop/settings.h src/ui/desktop/settings.cpp src/ui/desktop/settings_test.cpp src/ui/desktop/menu_actions.cpp src/ui/desktop/BUILD.bazel
git commit -m "refactor(ui): Settings saves through the shared FileActions (step 6c-2)"
```

### Task 3: `MainWindowServices` and the first half of `DesktopComposition`

Move `FileActions`, its four port adapters, its event sink, and `SystemLogger` with its thread out of `MainWindow` into `DesktopComposition`. `MainWindow` still builds the serial facade, remote utility, and logging engine (Task 5 moves them).

**Files:**
- Create: `src/ui/desktop/main_window_services.h`
- Modify: `src/ui/desktop/BUILD.bazel` (new `:main_window_services` target; `:desktop` depends on it)
- Modify: `src/ui/desktop/mainwindow.h` (constructor, members)
- Modify: `src/ui/desktop/mainwindow.cpp` (constructor)
- Modify: `src/ui/desktop/mainwindow_test.cpp` (fixture + four construction sites)
- Create: `apps/desktop/desktop_composition.h`, `apps/desktop/desktop_composition.cpp`, `apps/desktop/desktop_composition_test.cpp`
- Modify: `apps/desktop/BUILD.bazel`, `apps/desktop/main.cpp`

**Interfaces:**
- Consumes: `Settings(FileActions&, ...)` from Task 2 (no direct call here, but `menu_actions.cpp` now needs `*fileActions` to stay valid, which it does).
- Produces:
  - `struct MainWindowServices { FileActions& file_actions; QtFileRepository& config_repository; QtEventSink& file_action_events; SystemLogger& syslogger; };` in `src/ui/desktop/main_window_services.h`, target `//src/ui/desktop:main_window_services`.
  - `MainWindow::MainWindow(MainWindowServices services, const QString& peerAddress = "", const QString& peerPassword = "", QWidget *parent = nullptr)`.
  - `class DesktopComposition { explicit DesktopComposition(const QString& config_root = {}); ~DesktopComposition(); MainWindowServices services(); };` in `apps/desktop/desktop_composition.h`, target `//apps/desktop:composition`.

- [ ] **Step 1: Add `MainWindowServices`**

Create `src/ui/desktop/main_window_services.h`:

```cpp
#pragma once

class FileActions;
class QtEventSink;
class QtFileRepository;
class SystemLogger;

// Long-lived services MainWindow uses but does not own. A composition root
// (apps/desktop's DesktopComposition, or a test fixture) builds them, keeps
// them alive for MainWindow's whole lifetime, and passes this struct to its
// constructor.
struct MainWindowServices
{
    FileActions& file_actions; // set_base_dirs already applied
    QtFileRepository& config_repository;
    QtEventSink& file_action_events;
    SystemLogger& syslogger;
};
```

In `src/ui/desktop/BUILD.bazel`, add `load("@rules_cc//cc:cc_library.bzl", "cc_library")` as the first line and, before `qt_cc_library(name = "desktop", ...)`:

```python
# Forward declarations only; no Qt include, so a plain cc_library.
cc_library(
    name = "main_window_services",
    hdrs = ["main_window_services.h"],
)
```

and add `":main_window_services",` to `:desktop`'s `deps`. The package's `default_visibility` already includes `//apps/desktop:__pkg__`.

- [ ] **Step 2: Write the failing composition test**

Create `apps/desktop/desktop_composition_test.cpp`:

```cpp
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include "apps/desktop/desktop_composition.h"
#include "src/backend/definitions/file_actions.h"

class DesktopCompositionTest : public QObject
{
    Q_OBJECT

  private slots:
    void servicesReferToTheCompositionsOwnObjects()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{root.path()};

        const MainWindowServices first = composition.services();
        const MainWindowServices second = composition.services();

        QCOMPARE(&first.file_actions, &second.file_actions);
        QCOMPARE(&first.config_repository, &second.config_repository);
        QCOMPARE(&first.file_action_events, &second.file_action_events);
        QCOMPARE(&first.syslogger, &second.syslogger);
        QCOMPARE(first.file_actions.ConfigValuesStruct.base_config_directory, root.path());
    }

    // SystemLogger::run() spends its first second in a processEvents loop on
    // the syslog thread; the destructor must still stop and join it promptly.
    void destructionRightAfterConstructionDoesNotHang()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QElapsedTimer elapsed;
        elapsed.start();
        {
            DesktopComposition composition{root.path()};
        }
        QVERIFY2(elapsed.elapsed() < 5000, "composition teardown took longer than 5 s");
    }

    // main() rebuilds the composition on every RESTART_CODE iteration.
    void constructingTwiceInOneProcessSucceeds()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        for (int iteration = 0; iteration < 2; ++iteration)
        {
            DesktopComposition composition{root.path()};
            QCOMPARE(composition.services().file_actions.ConfigValuesStruct.base_config_directory, root.path());
        }
    }
};

QTEST_GUILESS_MAIN(DesktopCompositionTest)
#include "desktop_composition_test.moc"
```

Replace `apps/desktop/BUILD.bazel` with:

```python
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "fastecu_qttest", "qt_cc_binary", "qt_cc_library")

# Only the root //:fastecu alias consumes this composition root.
package(default_visibility = ["//:__pkg__"])

# Builds and owns MainWindow's long-lived services. Private to this package.
qt_cc_library(
    name = "composition",
    srcs = ["desktop_composition.cpp"],
    normal_hdrs = ["desktop_composition.h"],
    copts = COMMON_COPTS,
    visibility = ["//visibility:private"],
    deps = QT_DEPS + [
        "//src/backend/definitions",
        "//src/platform/desktop/common/logging",
        "//src/platform/desktop/common/ports",
        "//src/ui/desktop:main_window_services",
    ],
)

fastecu_qttest(
    name = "desktop_composition_test",
    size = "small",
    src = "desktop_composition_test.cpp",
    deps = [":composition"],
)

qt_cc_binary(
    name = "fastecu",
    srcs = ["main.cpp"],
    copts = COMMON_COPTS,
    # main.cpp includes no J2534 header, so the platform j2534 libraries are not
    # declared here. They reach the link transitively via //src/ui/desktop ->
    # //src/platform/desktop/common/serial:serial_qt_compat, whose own select()
    # picks unix vs windows. serial_qt_compat is TRANSITIONAL: if its deps change
    # or it is removed, add the platform select() back here or the Windows link
    # loses J2534 with no signal in this file.
    deps = [
        ":composition",
        "//src/ui/desktop",
    ],
)
```

If `qt_cc_library` rejects an empty `hdrs`, check its definition in `bazel/qt_common.bzl` and pass `hdrs = []` explicitly.

- [ ] **Step 3: Run it to verify it fails**

Run: `bazel test --config=release //apps/desktop:desktop_composition_test`
Expected: FAIL: `desktop_composition.h` / `desktop_composition.cpp` not found.

- [ ] **Step 4: Implement `DesktopComposition`**

Create `apps/desktop/desktop_composition.h`:

```cpp
#pragma once

#include <memory>

#include <QString>

#include "src/backend/definitions/file_actions.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/ui/desktop/main_window_services.h"

class QThread;
class SystemLogger;

// The desktop application's composition root: builds and owns every
// long-lived service MainWindow uses, and hands MainWindow non-owning
// references to them. Must outlive the MainWindow it serves.
class DesktopComposition
{
  public:
    // An empty config_root uses the platform's default FastECU directory.
    explicit DesktopComposition(const QString& config_root = {});
    ~DesktopComposition();

    DesktopComposition(const DesktopComposition&) = delete;
    DesktopComposition& operator=(const DesktopComposition&) = delete;

    MainWindowServices services();

  private:
    QtFileSystem file_system_;
    QtResourceBundle resource_bundle_;
    QtFileRepository file_repository_;
    QtAtomicFileWriter file_writer_;
    QtEventSink file_action_events_;
    FileActions file_actions_;
    std::unique_ptr<QThread> syslog_thread_;
    std::unique_ptr<SystemLogger> syslogger_;
};
```

Create `apps/desktop/desktop_composition.cpp`:

```cpp
#include "apps/desktop/desktop_composition.h"

#include <QThread>

#include "src/platform/desktop/common/logging/systemlogger.h"

DesktopComposition::DesktopComposition(const QString& config_root)
    : file_actions_(file_system_, resource_bundle_, file_repository_, file_writer_, file_action_events_)
{
    FileActions::ConfigValuesStructure *config = &file_actions_.ConfigValuesStruct;
    file_actions_.set_base_dirs(config,
                                (config_root.isEmpty() ? config->base_config_directory : config_root).toStdString());

    syslog_thread_ = std::make_unique<QThread>();
    syslogger_ = std::make_unique<SystemLogger>(config->syslog_files_directory, config->software_name,
                                                config->software_version);
    syslogger_->moveToThread(syslog_thread_.get());
    QObject::connect(syslog_thread_.get(), &QThread::started, syslogger_.get(), &SystemLogger::run);
    syslog_thread_->start();
}

DesktopComposition::~DesktopComposition()
{
    // The logger lives on its own thread; stop and join it before deleting
    // it. (Before step 6c neither was ever stopped: SystemLogger::finished,
    // which the old wiring waited on, is never emitted.)
    syslog_thread_->quit();
    syslog_thread_->wait();
    syslogger_.reset();
}

MainWindowServices DesktopComposition::services()
{
    return {
        .file_actions = file_actions_,
        .config_repository = file_repository_,
        .file_action_events = file_action_events_,
        .syslogger = *syslogger_,
    };
}
```

- [ ] **Step 5: Run the composition test to verify it passes**

Run: `bazel test --config=release //apps/desktop:desktop_composition_test`
Expected: PASS, three test functions.

- [ ] **Step 6: Switch `MainWindow` to the injected services**

In `src/ui/desktop/mainwindow.h`:
- Add `#include "src/ui/desktop/main_window_services.h"` next to the other `src/ui/desktop/` includes.
- Replace the constructor declaration and its comment with:

```cpp
    MainWindow(MainWindowServices services, const QString& peerAddress = "", const QString& peerPassword = "",
               QWidget *parent = nullptr);
```

- Add `MainWindowServices services_;` as the **first** member after `private:` (before `enum { _LOG_E ...`), so it is initialized before everything the constructor body uses.
- Delete the members `QtFileSystem m_configFileSystem;`, `QtResourceBundle m_configResourceBundle;`, `QtFileRepository m_configFileRepository;`, `QtAtomicFileWriter m_definitionFileWriter;`, and `QtEventSink fileActionsEvents_{this};`.
- Change `std::unique_ptr<FileActions> fileActions;` to `FileActions *fileActions = nullptr;`.
- Keep `SystemLogger *syslogger;` (now non-owning).
- Delete the includes of `qt_atomic_file_writer.h`, `qt_file_system.h`, and `qt_resource_bundle.h` only if nothing else in `mainwindow.h`/`mainwindow.cpp` still names those types (grep first); keep `qt_file_repository.h` and `qt_event_sink.h`, which `mainwindow.cpp` now uses through `services_`.

In `src/ui/desktop/mainwindow.cpp`, change the constructor head:

```cpp
MainWindow::MainWindow(MainWindowServices services, const QString& peerAddress, const QString& peerPassword,
                       QWidget *parent)
    : QMainWindow(parent), services_(services), peerAddress(peerAddress), peerPassword(peerPassword),
      ui{std::make_unique<Ui::MainWindow>()}
```

Replace

```cpp
    fileActions = std::make_unique<FileActions>(m_configFileSystem, m_configResourceBundle, m_configFileRepository,
                                                m_definitionFileWriter, fileActionsEvents_);
    configValues = &fileActions->ConfigValuesStruct;

    fileActions->set_base_dirs(
        configValues, (config_root.isEmpty() ? configValues->base_config_directory : config_root).toStdString());
```

with

```cpp
    fileActions = &services_.file_actions;
    configValues = &fileActions->ConfigValuesStruct;
```

Replace the syslogger block (from `QThread *syslog_thread = new QThread();` through `syslog_thread->start();`) with:

```cpp
    syslogger = &services_.syslogger;
    QObject::connect(this, &MainWindow::LOG_E, syslogger, &SystemLogger::log_messages);
    QObject::connect(this, &MainWindow::LOG_W, syslogger, &SystemLogger::log_messages);
    QObject::connect(this, &MainWindow::LOG_I, syslogger, &SystemLogger::log_messages);
    QObject::connect(this, &MainWindow::LOG_D, syslogger, &SystemLogger::log_messages);
    QObject::connect(this, &MainWindow::enable_log_write_to_file, syslogger, &SystemLogger::enable_log_write_to_file);
    QObject::connect(syslogger, &SystemLogger::send_message_to_log_window, this,
                     &MainWindow::send_message_to_log_window);
```

(The removed `finished`/`started` connections now live in `DesktopComposition`, or were dead.)

Then replace every remaining use:
- `&fileActionsEvents_` → `&services_.file_action_events` (two `QObject::connect` calls, `logged` and `noticed`).
- `m_configFileRepository` → `services_.config_repository` (the `DefinitionAuthoringDialog` construction and the `load_menu_definition` call).

Run: `grep -n "m_configFileSystem\|m_configResourceBundle\|m_configFileRepository\|m_definitionFileWriter\|fileActionsEvents_\|config_root" src/ui/desktop/*.cpp src/ui/desktop/*.h`
Expected: no matches.

- [ ] **Step 7: Switch `main.cpp` to the composition**

In `apps/desktop/main.cpp`, add `#include "apps/desktop/desktop_composition.h"` after `#include "src/ui/desktop/mainwindow.h"`, and replace

```cpp
        MainWindow w(addr, password, nullptr);
```

with

```cpp
        // Declared before the window so it outlives it: MainWindow holds
        // references into the composition until it is destroyed.
        DesktopComposition composition;
        MainWindow w(composition.services(), addr, password);
```

- [ ] **Step 8: Update `mainwindow_test` to inject services**

In `src/ui/desktop/mainwindow_test.cpp`, add these includes after the `fake_backend.h` include:

```cpp
#include "src/platform/desktop/common/logging/systemlogger.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
```

Add to the anonymous namespace, after `writeTextFile`:

```cpp
// The services DesktopComposition builds in the real app, minus the syslog
// thread: the logger lives on the test thread, which is enough for a receiver.
struct TestServices
{
    explicit TestServices(const QString& config_root)
        : file_actions(file_system, resource_bundle, file_repository, file_writer, events)
    {
        FileActions::ConfigValuesStructure *config = &file_actions.ConfigValuesStruct;
        file_actions.set_base_dirs(config, config_root.toStdString());
        syslogger = std::make_unique<SystemLogger>(config->syslog_files_directory, config->software_name,
                                                   config->software_version);
    }

    MainWindowServices services()
    {
        return {
            .file_actions = file_actions,
            .config_repository = file_repository,
            .file_action_events = events,
            .syslogger = *syslogger,
        };
    }

    QtFileSystem file_system;
    QtResourceBundle resource_bundle;
    QtFileRepository file_repository;
    QtAtomicFileWriter file_writer;
    QtEventSink events;
    FileActions file_actions;
    std::unique_ptr<SystemLogger> syslogger;
};
```

Replace each of the four occurrences of

```cpp
        MainWindow window{"", "", nullptr, config_root_.path()};
```

with

```cpp
        TestServices services{config_root_.path()};
        MainWindow window{services.services()};
```

(`services` is declared first, so it outlives `window`.) Leave the `window.serial` overwrites alone; Task 5 removes them.

Add `"//src/platform/desktop/common/logging",` and `"//src/platform/desktop/common/ports",` to `test_mainwindow`'s `deps` in `src/ui/desktop/BUILD.bazel`. Both packages' visibility already admits `//src/ui/desktop`.

- [ ] **Step 9: Build and run everything affected**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //apps/... //src/ui/... //tests/...`
Expected: build succeeds and all tests PASS, including `explicitConfigRootLoadsFixtureAndProvisionsDirectories` (it now proves that the fixture's `set_base_dirs` plus `MainWindow`'s own `check_config_dirs` still provision the directories).

- [ ] **Step 10: Commit**

```bash
git add src/ui/desktop apps/desktop
git commit -m "refactor(desktop): add DesktopComposition and inject FileActions and the syslogger (step 6c-2)"
```

- [ ] **Step 11: Gate and PR (when authorized)**

Run `prek run --all-files`, `bazel test --config=release //...`, `bazel run //:clang_tidy_report_changed`; all must pass. Then push and open a PR titled `refactor(desktop): composition root, part 1 (step 6c-2)`. The PR body calls out the syslogger-thread shutdown and the Settings sink change, both from the spec.

---

## PR 6c-3

Branch from `master` after PR 6c-2 merges:

```bash
git switch master && git pull --ff-only && git switch -c refactor/step6c-3-composition-root-facades
```

### Task 4: `desktop_serial_factory`

A constructor-only entry point to `SerialPortActions` for `apps/desktop`, which may not join `serial_qt_compat`'s frozen visibility list.

**Files:**
- Create: `src/platform/desktop/common/serial/desktop_serial_factory.h`
- Create: `src/platform/desktop/common/serial/desktop_serial_factory.cpp`
- Create: `src/platform/desktop/common/serial/desktop_serial_factory_test.cpp`
- Modify: `src/platform/desktop/common/serial/BUILD.bazel`

**Interfaces:**
- Consumes: `SerialPortActions(QString peerAddress, QString password, QWebSocket *web_socket, QObject *parent, std::function<SerialBackend *()> backendFactoryForTests = {})` and its signals `LOG_E/LOG_W/LOG_I/LOG_D(QString, bool, bool)`.
- Produces:
  - `struct SerialPortActionsDeleter { void operator()(SerialPortActions *) const; };`
  - `using OwnedSerialPortActions = std::unique_ptr<SerialPortActions, SerialPortActionsDeleter>;`
  - `OwnedSerialPortActions make_serial_port_actions(const QString& peer_address, const QString& peer_password, QObject& log_sink);`
  - target `//src/platform/desktop/common/serial:desktop_serial_factory`, visible to `//apps/desktop:__pkg__`.

- [ ] **Step 1: Write the failing test**

Create `src/platform/desktop/common/serial/desktop_serial_factory_test.cpp`:

```cpp
#include <QStringList>
#include <QTest>

#include "src/platform/desktop/common/serial/desktop_serial_factory.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

class RecordingLogSink : public QObject
{
    Q_OBJECT

  public:
    QStringList messages;

  public slots:
    void log_messages(const QString& message, bool /*timestamp*/, bool /*linefeed*/)
    {
        messages << message;
    }
};

class DesktopSerialFactoryTest : public QObject
{
    Q_OBJECT

  private slots:
    void buildsADirectFacadeWhenNoPeerIsGiven()
    {
        RecordingLogSink sink;
        const OwnedSerialPortActions serial = make_serial_port_actions({}, {}, sink);
        QVERIFY(serial != nullptr);
        QVERIFY(serial->isDirectConnection());
    }

    void everyLogLevelReachesTheSink()
    {
        RecordingLogSink sink;
        const OwnedSerialPortActions serial = make_serial_port_actions({}, {}, sink);
        QVERIFY(serial != nullptr);
        sink.messages.clear();

        emit serial->LOG_E("error", false, false);
        emit serial->LOG_W("warning", false, false);
        emit serial->LOG_I("info", false, false);
        emit serial->LOG_D("debug", false, false);

        QCOMPARE(sink.messages, (QStringList{"error", "warning", "info", "debug"}));
    }
};

QTEST_GUILESS_MAIN(DesktopSerialFactoryTest)
#include "desktop_serial_factory_test.moc"
```

Add to `src/platform/desktop/common/serial/BUILD.bazel`, after the `serial_qt_compat` target:

```python
# Constructor-only entry point to SerialPortActions for the desktop
# composition root. serial_qt_compat's visibility is a frozen, shrink-only
# allowlist; this target lets //apps/desktop own the facade without joining
# that list or seeing the facade's API.
qt_cc_library(
    name = "desktop_serial_factory",
    srcs = ["desktop_serial_factory.cpp"],
    normal_hdrs = ["desktop_serial_factory.h"],
    copts = COMMON_COPTS,
    visibility = ["//apps/desktop:__pkg__"],
    deps = QT_DEPS + [":serial_qt_compat"],
)

fastecu_qttest(
    name = "desktop_serial_factory_test",
    size = "small",
    src = "desktop_serial_factory_test.cpp",
    deps = [
        ":desktop_serial_factory",
        ":serial_qt_compat",
    ],
)
```

If `qt_cc_library` rejects a target with no `hdrs`, pass `hdrs = []` explicitly (as in Task 3).

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test --config=release //src/platform/desktop/common/serial:desktop_serial_factory_test`
Expected: FAIL: `desktop_serial_factory.h` not found.

- [ ] **Step 3: Implement the factory**

Create `src/platform/desktop/common/serial/desktop_serial_factory.h`:

```cpp
#pragma once

#include <memory>

#include <QString>

class QObject;
class SerialPortActions;

// Constructor-only entry point to the serial facade for the desktop
// composition root; see this target's BUILD.bazel comment for why it exists.
// SerialPortActions stays an incomplete type for callers.
struct SerialPortActionsDeleter
{
    void operator()(SerialPortActions *serial) const;
};

using OwnedSerialPortActions = std::unique_ptr<SerialPortActions, SerialPortActionsDeleter>;

// Builds the facade (direct when peer_address is empty, remote otherwise) and
// routes its LOG_E/LOG_W/LOG_I/LOG_D signals to log_sink's
// log_messages(QString, bool, bool) slot.
OwnedSerialPortActions make_serial_port_actions(const QString& peer_address, const QString& peer_password,
                                                QObject& log_sink);
```

Create `src/platform/desktop/common/serial/desktop_serial_factory.cpp`:

```cpp
#include "src/platform/desktop/common/serial/desktop_serial_factory.h"

#include <array>

#include "src/platform/desktop/common/serial/serial_port_actions.h"

void SerialPortActionsDeleter::operator()(SerialPortActions *serial) const
{
    delete serial;
}

OwnedSerialPortActions make_serial_port_actions(const QString& peer_address, const QString& peer_password,
                                                QObject& log_sink)
{
    auto serial = std::make_unique<SerialPortActions>(peer_address, peer_password, nullptr, nullptr);
    // String-based connections, so log_sink can be any QObject with a
    // log_messages(QString, bool, bool) slot (SystemLogger in production).
    // SystemLogger::log_messages reads sender()'s signal to pick the level,
    // which a direct signal-to-slot connection preserves.
    constexpr auto kLogSignals = std::to_array<const char *>({
        SIGNAL(LOG_E(QString, bool, bool)),
        SIGNAL(LOG_W(QString, bool, bool)),
        SIGNAL(LOG_I(QString, bool, bool)),
        SIGNAL(LOG_D(QString, bool, bool)),
    });
    for (const char *signal : kLogSignals)
    {
        QObject::connect(serial.get(), signal, &log_sink, SLOT(log_messages(QString, bool, bool)));
    }
    return OwnedSerialPortActions{serial.release()};
}
```

If `constexpr` is rejected because `SIGNAL(...)` expands to a non-constant expression on some compiler, change `constexpr auto` to `const auto`.

- [ ] **Step 4: Run it to verify it passes, and confirm the allowlist is unchanged**

Run: `bazel test --config=release //src/platform/desktop/common/serial:all //:serial_compat_allowlist`
Expected: PASS. `serial_compat_allowlist` must pass untouched: the new target is not a consumer outside the package.

- [ ] **Step 5: Commit**

```bash
git add src/platform/desktop/common/serial
git commit -m "feat(serial): constructor-only serial facade factory for the composition root (step 6c-3)"
```

### Task 5: Move the serial facade, remote utility, clock, and logging engine into the composition

**Files:**
- Modify: `src/ui/desktop/main_window_services.h`
- Modify: `apps/desktop/desktop_composition.h`, `apps/desktop/desktop_composition.cpp`, `apps/desktop/desktop_composition_test.cpp`, `apps/desktop/BUILD.bazel`, `apps/desktop/main.cpp`
- Modify: `src/ui/desktop/mainwindow.h`, `src/ui/desktop/mainwindow.cpp` (constructor at the serial/remote construction, `setupLoggingEngine()`)
- Modify: `src/ui/desktop/mainwindow_test.cpp`, `src/ui/desktop/BUILD.bazel` (`test_mainwindow` deps)

**Interfaces:**
- Consumes: `make_serial_port_actions` and `OwnedSerialPortActions` from Task 4; `MainWindowServices` and `DesktopComposition` from Task 3; `RemoteUtility(const QString& peerAddress, QString password, QWebSocket *web_socket = nullptr, QObject *parent = nullptr)`; `fastecu::desktop::logging::LoggingEngine(QObject *parent = nullptr)`.
- Produces:
  - `MainWindowServices` gains `SerialPortActions& serial; RemoteUtility& remote_utility; fastecu::desktop::logging::LoggingEngine& logging_engine; QtClock& logging_clock;` (in that order, after `syslogger`).
  - `MainWindow::MainWindow(MainWindowServices services, const QString& peerAddress = "", QWidget *parent = nullptr)` (drops `peerPassword`).
  - `DesktopComposition(const QString& peer_address, const QString& peer_password, const QString& config_root = {})`.

- [ ] **Step 1: Extend the composition test (failing)**

In `apps/desktop/desktop_composition_test.cpp`:
- Change every `DesktopComposition composition{root.path()};` to `DesktopComposition composition{{}, {}, root.path()};`.
- Append these comparisons to `servicesReferToTheCompositionsOwnObjects`:

```cpp
        QCOMPARE(&first.serial, &second.serial);
        QCOMPARE(&first.remote_utility, &second.remote_utility);
        QCOMPARE(&first.logging_engine, &second.logging_engine);
        QCOMPARE(&first.logging_clock, &second.logging_clock);
```

Run: `bazel test --config=release //apps/desktop:desktop_composition_test`
Expected: FAIL to compile: no constructor taking three arguments, and `MainWindowServices` has no member `serial`.

- [ ] **Step 2: Extend `MainWindowServices`**

In `src/ui/desktop/main_window_services.h`, add forward declarations:

```cpp
class QtClock;
class RemoteUtility;
class SerialPortActions;
namespace fastecu::desktop::logging
{
class LoggingEngine;
}
```

and the members after `syslogger`:

```cpp
    SerialPortActions& serial;
    RemoteUtility& remote_utility;
    fastecu::desktop::logging::LoggingEngine& logging_engine;
    QtClock& logging_clock;
```

- [ ] **Step 3: Extend `DesktopComposition`**

In `apps/desktop/desktop_composition.h`:
- Add `#include "src/platform/desktop/common/ports/qt_clock.h"` and `#include "src/platform/desktop/common/serial/desktop_serial_factory.h"`.
- Add forward declarations `class RemoteUtility;` and `namespace fastecu::desktop::logging { class LoggingEngine; }`.
- Change the constructor to `DesktopComposition(const QString& peer_address, const QString& peer_password, const QString& config_root = {});` (no longer `explicit`-worthy with three parameters; drop `explicit`).
- Append members after `syslogger_`:

```cpp
    OwnedSerialPortActions serial_;
    std::unique_ptr<RemoteUtility> remote_utility_;
    QtClock logging_clock_;
    std::unique_ptr<fastecu::desktop::logging::LoggingEngine> logging_engine_;
```

In `apps/desktop/desktop_composition.cpp`:
- Add `#include "src/platform/desktop/common/logging/logging_engine.h"` and `#include "src/platform/desktop/common/remote_utility/remote_utility.h"`.
- Change the constructor signature to `DesktopComposition::DesktopComposition(const QString& peer_address, const QString& peer_password, const QString& config_root)` and append to its body, after `syslog_thread_->start();`:

```cpp
    serial_ = make_serial_port_actions(peer_address, peer_password, *syslogger_);
    remote_utility_ = std::make_unique<RemoteUtility>(peer_address, peer_password, nullptr, nullptr);

    using fastecu::desktop::logging::LoggingEngine;
    logging_engine_ = std::make_unique<LoggingEngine>();
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_E, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_W, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_I, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_D, syslogger_.get(), &SystemLogger::log_messages);
```

- Replace the destructor body with:

```cpp
    // Dependents first: the engine's transports reference the serial facade,
    // and every service logs to the syslogger.
    logging_engine_.reset();
    remote_utility_.reset();
    serial_.reset();
    // The logger lives on its own thread; stop and join it before deleting
    // it. (Before step 6c neither was ever stopped: SystemLogger::finished,
    // which the old wiring waited on, is never emitted.)
    syslog_thread_->quit();
    syslog_thread_->wait();
    syslogger_.reset();
```

- Extend `services()`:

```cpp
    return {
        .file_actions = file_actions_,
        .config_repository = file_repository_,
        .file_action_events = file_action_events_,
        .syslogger = *syslogger_,
        .serial = *serial_,
        .remote_utility = *remote_utility_,
        .logging_engine = *logging_engine_,
        .logging_clock = logging_clock_,
    };
```

(`*serial_` binds a reference to the incomplete `SerialPortActions`; that is valid C++.)

In `apps/desktop/BUILD.bazel`, add to `:composition`'s `deps`:

```python
        "//src/platform/desktop/common/logging:logging_runtime",
        "//src/platform/desktop/common/remote_utility",
        "//src/platform/desktop/common/serial:desktop_serial_factory",
```

In `apps/desktop/main.cpp`, change `DesktopComposition composition;` to `DesktopComposition composition{addr, password};`.

- [ ] **Step 4: Run the composition test to verify it passes**

Run: `bazel test --config=release //apps/desktop:desktop_composition_test`
Expected: PASS, three test functions. (`//:fastecu` does not build yet; Step 5 fixes `MainWindow`.)

- [ ] **Step 5: Switch `MainWindow` to the injected facades**

In `src/ui/desktop/mainwindow.h`:
- Change the constructor to `MainWindow(MainWindowServices services, const QString& peerAddress = "", QWidget *parent = nullptr);`.
- Delete the member `QString peerPassword;` and `QtClock m_loggingClock;`.
- Keep `SerialPortActions *serial;`, `RemoteUtility *remote_utility;`, and `fastecu::desktop::logging::LoggingEngine *loggingEngine = nullptr;`; they are now non-owning. Initialize `serial` and `remote_utility` to `nullptr` in their declarations.

In `src/ui/desktop/mainwindow.cpp`, change the constructor head to:

```cpp
MainWindow::MainWindow(MainWindowServices services, const QString& peerAddress, QWidget *parent)
    : QMainWindow(parent), services_(services), peerAddress(peerAddress), ui{std::make_unique<Ui::MainWindow>()}
```

Replace

```cpp
    serial = new SerialPortActions(peerAddress, peerPassword, nullptr, this);
    QObject::connect(serial, &SerialPortActions::LOG_E, syslogger, &SystemLogger::log_messages);
    QObject::connect(serial, &SerialPortActions::LOG_W, syslogger, &SystemLogger::log_messages);
    QObject::connect(serial, &SerialPortActions::LOG_I, syslogger, &SystemLogger::log_messages);
    QObject::connect(serial, &SerialPortActions::LOG_D, syslogger, &SystemLogger::log_messages);

    remote_utility = new RemoteUtility(peerAddress, peerPassword, nullptr, this);
```

with

```cpp
    serial = &services_.serial;
    remote_utility = &services_.remote_utility;
```

In `setupLoggingEngine()`, replace

```cpp
    loggingEngine = new fastecu::desktop::logging::LoggingEngine(this);
```

with

```cpp
    loggingEngine = &services_.logging_engine;
```

and delete its four `connect(loggingEngine, &...::LOG_x, syslogger, ...)` lines (the composition wires them). Keep the `valuesUpdated`/`sessionEnded` connections and all three `registerProtocol` calls. In the SSM factory, change `m_loggingClock` to `services_.logging_clock`.

Run: `grep -n "peerPassword\|m_loggingClock\|new SerialPortActions\|new RemoteUtility\|new fastecu::desktop::logging::LoggingEngine" src/ui/desktop/*.cpp src/ui/desktop/*.h`
Expected: no matches.

- [ ] **Step 6: Update `mainwindow_test` to own the fake-backed facade**

In `src/ui/desktop/mainwindow_test.cpp`:
- Add includes: `src/platform/desktop/common/logging/logging_engine.h`, `src/platform/desktop/common/ports/qt_clock.h`, `src/platform/desktop/common/remote_utility/remote_utility.h`.
- Move `TestServices` below `fakeSerial` in the anonymous namespace, and extend it. Add to the end of its constructor body:

```cpp
        serial = fakeSerial(nullptr, &fake);
```

Add to its members, after `syslogger`:

```cpp
    FakeBackend *fake = nullptr;
    std::unique_ptr<SerialPortActions> serial; // null if the fake backend failed to start
    RemoteUtility remote_utility{"", ""};
    QtClock logging_clock;
    fastecu::desktop::logging::LoggingEngine logging_engine;
```

and to `services()`:

```cpp
            .serial = *serial,
            .remote_utility = remote_utility,
            .logging_engine = logging_engine,
            .logging_clock = logging_clock,
```

(`logging_engine` is declared after `serial`, so it is destroyed first, as in the composition.)

- In each of the four tests, directly after `TestServices services{config_root_.path()};`, add `QVERIFY(services.serial != nullptr);` so `services()` is never called with a null facade.
- In the three tests that replace the serial facade, delete these five lines:

```cpp
        FakeBackend *fake = nullptr;
        std::unique_ptr<SerialPortActions> serial = fakeSerial(&window, &fake);
        QVERIFY(serial != nullptr);
        delete window.serial;
        window.serial = serial.release();
```

and replace them with `FakeBackend *fake = services.fake;`, so the following `EXPECT_CALL(*fake, ...)` lines are unchanged. Expectations are still set after `MainWindow` is constructed, so construction-time calls into the `NiceFakeBackend` are not counted against them.

Add `"//src/platform/desktop/common/logging:logging_runtime",` and `"//src/platform/desktop/common/remote_utility",` to `test_mainwindow`'s `deps`.

- [ ] **Step 7: Build and run everything**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //...`
Expected: build succeeds and every test PASSES, including `//:portable_closure` and `//:serial_compat_allowlist` unchanged.

If a `test_mainwindow` case now fails because `MainWindow`'s constructor behaves differently against the fake backend than against the real direct facade it used to build (for example, an empty port list changes a default), note which call differs and give it a matching `ON_CALL` default in `TestServices`'s constructor. Do not weaken an `EXPECT_CALL`.

- [ ] **Step 8: Manual smoke (macOS)**

1. `bazel run --config=release //:fastecu`: the startup splash and main window appear. Open Edit → Settings, close it, and confirm `~/…/FastECU/<version>/config/fastecu.cfg` has a fresh modification time. Quit: the process exits within a couple of seconds.
2. Start the remote utility host (see `src/platform/desktop/common/remote_utility/`), then `bazel run --config=release //:fastecu -- -s 127.0.0.1:33314`: the "Waiting for peer" network splash appears and the window opens after the connection. Stop the host: the "Network connection lost" restart prompt appears; choose Restart and confirm the app comes back up (a second composition in one process).

Record the results in the PR description. If the remote host is unavailable, say so there; do not claim step 2 ran.

- [ ] **Step 9: Commit**

```bash
git add src/ui/desktop apps/desktop
git commit -m "refactor(desktop): composition root owns the serial facade, remote utility, and logging engine (step 6c-3)"
```

- [ ] **Step 10: Gate and PR (when authorized)**

Run `prek run --all-files`, `bazel test --config=release //...`, `bazel run //:clang_tidy_report_changed`; all must pass. Push and open a PR titled `refactor(desktop): composition root, part 2 (step 6c-3)`, including the manual smoke results.

---

## PR 6c-4

Branch from `master` after PR 6c-3 merges:

```bash
git switch master && git pull --ff-only && git switch -c docs/step6c-4-composition-root-closeout
```

### Task 6: Close out step 6c in the docs

**Files:**
- Modify: `docs/modularization-plan.md` (Status section; step 6 list)
- Modify: `docs/tech-debt.md` ("P1: Separate UI from application logic")
- Modify: `docs/design-notes.md` (new section)
- Delete: `docs/superpowers/specs/2026-09-26-step6c-composition-root-design.md`, `docs/superpowers/plans/2026-09-26-step6c-composition-root.md`

**Interfaces:**
- Consumes: the merged PR numbers for 6c-1, 6c-2, and 6c-3 (read them with `gh pr list --state merged --search "step 6c" --json number,title`).
- Produces: nothing.

- [ ] **Step 1: Modularization plan**

In `docs/modularization-plan.md`:
- In the Status paragraph beginning "Step 6 (thin desktop shell) is under way", change "6a (de-widget `FileActions`) and 6b (calibration map-edit use case) are complete" to "6a (de-widget `FileActions`), 6b (calibration map-edit use case), and 6c (desktop composition root) are complete".
- In step 6's list, replace the bullet "Move construction and platform selection into `apps/desktop`." with:

```markdown
   - **6c desktop composition root — complete.** `apps/desktop`'s
     `DesktopComposition` builds and owns `FileActions` and its port
     adapters, the syslogger and its thread, the serial facade, the remote
     utility, the logging clock, and the logging engine, and passes
     `MainWindow` a `MainWindowServices` struct of references (#<6c-2>,
     #<6c-3>). The serial facade is reached through the constructor-only
     `//src/platform/desktop/common/serial:desktop_serial_factory`, so the
     frozen `serial_qt_compat` allowlist did not grow. The dead
     `EcuOperations` class was deleted (#<6c-1>). Logging-protocol
     registration stays in `MainWindow`; see the tech-debt roadmap.
```

substituting the real PR numbers.

- [ ] **Step 2: Tech-debt roadmap**

In `docs/tech-debt.md`, under "P1: Separate UI from application logic" → "Actions", add:

```markdown
- Move logging-protocol registration (`MainWindow::setupLoggingEngine()`'s
  three `registerProtocol` calls) into the composition root. It stayed in
  `MainWindow` in step 6c because the SSM factory reads `ecu_radio_button`
  to pick ECU vs TCU; moving it needs that choice passed in as data (for
  example a `target_is_ecu` field in the logging snapshot) rather than read
  from a widget inside the factory.
```

Also update the "`mainwindow.cpp` is about 2.7k lines" figure in that section's first paragraph to the current `wc -l src/ui/desktop/mainwindow.cpp` result.

- [ ] **Step 3: Design notes**

Add a section to `docs/design-notes.md`, following that file's existing heading style:

```markdown
## Step 6c: desktop composition root

- `DesktopComposition` is declared before `MainWindow` in `main()`, inside
  the `RESTART_CODE` loop, so every restart rebuilds both and the
  composition always outlives the window that references it.
- Its destructor releases dependents first (engine, remote utility, serial
  facade), then quits and joins the syslogger thread. Before 6c that thread
  was never stopped — `SystemLogger::finished` is never emitted — and every
  restart leaked one.
- `apps/desktop` must not join `serial_qt_compat`'s frozen visibility list,
  so it builds the facade through `desktop_serial_factory`, which exposes
  construction only and keeps `SerialPortActions` an incomplete type in
  `apps/desktop`. The factory uses string-based `SIGNAL`/`SLOT` connections
  to the log sink; `desktop_serial_factory_test` is what catches a broken
  one.
- `Settings` used to save through a throwaway `FileActions` reporting to a
  `NullEventSink`; it now uses the shared instance, so save diagnostics
  reach the log window.
```

- [ ] **Step 4: Delete the spec and plan**

```bash
git rm docs/superpowers/specs/2026-09-26-step6c-composition-root-design.md docs/superpowers/plans/2026-09-26-step6c-composition-root.md
```

(Precedent: #362 distilled the completed step 5 and 6 specs into the design notes and deleted them.)

- [ ] **Step 5: Check links and commit**

Run: `prek run --all-files`
Expected: PASS (lychee finds no broken links to the deleted files; if it does, fix the linking text).

```bash
git add docs
git commit -m "docs: close out step 6c (desktop composition root)"
```

- [ ] **Step 6: PR (when authorized)**

Push and open a PR titled `docs: close out step 6c (desktop composition root)`.
