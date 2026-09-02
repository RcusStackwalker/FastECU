# Step 6a-3 FileActions Event Sink Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drop `QWidget`/`Q_OBJECT`/the four `LOG_*` signals from `FileActions`, inject a `fastecu::IEventSink&` in their place, convert its 12 `QMessageBox::warning` sites and 25 `emit LOG_*` sites to `events_.notice(...)`/`events_.log(...)`, remove its 13 `tr()` calls, and rewire every caller (production and test) so `bazel test --config=release //...` stays green throughout.

**Architecture:** `FileActions` becomes a plain class holding a `fastecu::IEventSink&` alongside its existing injected ports (`IFileSystem`, `IResourceBundle`, `IFileRepository`, `IAtomicFileWriter`). Every `emit LOG_X(msg, true, true)` becomes `events_.log(fastecu::LogLevel::X, ...)`; every `QMessageBox::warning(this, title, text)` becomes `events_.notice((title + ": " + text).toStdString())`, preserving the dialog title as a prefix on the notice body rather than as a separate field, since `IEventSink::notice` (and the `QtEventSink::noticed(QString)` signal it produces) carries one string. `MainWindow` owns a `QtEventSink` member, wires its `logged`/`noticed` signals to the log window and a `QMessageBox::warning`, and holds `FileActions` in a `std::unique_ptr` now that nothing parents it. A discovered prerequisite not in the design doc: `@rules_qt//:qt_widgets` reaches `//src/backend/definitions:definitions` today not just through `FileActions` itself but through five sibling `qt_cc_library` targets and four other backend packages' "legacy adapter" targets, all of which build their `deps` from the same `QT_DEPS` bundle that always includes widgets. Task 1 introduces a widgets-free bundle and repoints every one of those onto it.

**Tech Stack:** C++23, Bazel 9.1.1, GoogleTest/GoogleMock (via `fastecu_gtest`/`fastecu_qttest`), Qt 6.8.3 (Core/Xml only, once this lands — no more Widgets in this package).

**Spec:** [docs/superpowers/specs/2026-09-01-step6a-file-actions-dewidget-design.md](../specs/2026-09-01-step6a-file-actions-dewidget-design.md) — this plan implements "Move c" (message boxes / signals) and the `QWidget`/`Q_OBJECT` removal called out under "What changes", i.e. slice **6a-3**. Slices 6a-1 (menu split) and 6a-2 (definition authoring dialog) are already merged (`f74c5255`, `be4bfcb5`); 6a-4 (expression shim drain) and 6a-5 (`//:backend_no_widgets` guard) remain after this.

## Global Constraints

- **Do not edit any file under `src/platform/desktop/common/flash/legacy/` or `src/ui/desktop/flash/`.** The flash drain owns those paths on a parallel branch. Verify with `git diff --name-only origin/master | grep -E 'src/platform/desktop/common/flash/legacy/|src/ui/desktop/flash/'` — it must print nothing.
- `src/ui/desktop/mainwindow.{h,cpp}` are also edited by that branch, specifically its constructor's wiring region (lines 62-123 as of `be4bfcb5`) and the flash-dialog include block/dispatch branches lower down. Keep edits there minimal and localized to the `fileActions`/log-wiring lines; no reformatting or tidying of untouched code.
- **The `FileActions::` type and static surface stays frozen**: `ConfigValuesStructure`, `LogValuesStructure`, `EcuCalDefStructure`, `RomInfoEnum`, `parse_nrc_message`/`parse_dtc_message`, and the `validate_*`/`collect_ecuflash_*` statics keep their exact names and signatures. ~85 files depend on it, most of them drain-owned.
- **Do not remove `QDomDocument`, `QXmlStreamReader`, `QDebug`, `QElapsedTimer`, or `QDateTime` usage** — only the widget-only includes (`QApplication`, `QWidget`, `QScreen`, `QFileDialog`, `QMessageBox`, `QLabel`, `QComboBox`) go, and only after confirming with grep that nothing else in the three `.cpp` files still needs them.
- Layering: `ui → backend` is permitted, `backend → ui` never is.
- Every header needs `#pragma once` (enforced by prek).
- Backend results are `fastecu::Result<T>`/`fastecu::Status`, checked with `.has_value()`, never the implicit `operator bool`.
- Run `prek run --all-files` before each commit. Build and test with `--config=release`.
- Every `emit LOG_*` site in `file_actions.cpp`, `file_actions_romraider.cpp`, and `file_actions_ecuflash.cpp` already passes `(message, true, true)` — no site uses a different timestamp/linefeed combination, so dropping those two parameters (which `IEventSink::log` has no room for) loses no information. Verified by exhaustive grep against the current tree; if a future edit to this plan finds a counter-example, stop and re-derive rather than assuming this still holds.

---

## Discovered prerequisite: `QT_DEPS` always drags in `@rules_qt//:qt_widgets`

`bazel/qt_targets.bzl`'s `QT_DEPS` list (charts, core, gui, remote_objects, serial_port, test, web_sockets, **widgets**, xml) is the *only* Qt dependency bundle `qt_cc_library` targets in this codebase use — there is no lighter variant. Verified with `bazel cquery --config=release 'somepath(<target>, @rules_qt//:qt_widgets)'` against the tree at `be4bfcb5`, before any edits: the following targets each pull in `@rules_qt//:qt_widgets` **directly** through their own `deps = QT_DEPS + [...]` line, none of them referencing any widget class (confirmed by grepping each header/source for `QWidget|QDialog|QMessageBox|QFileDialog|QPushButton|QLabel|QComboBox|QLineEdit|QTextEdit|QGridLayout|Q_OBJECT` — all clean):

| Target | File |
|---|---|
| `//src/backend/definitions:config_values` | `src/backend/definitions/BUILD.bazel` |
| `//src/backend/definitions:ecu_cal_def` | `src/backend/definitions/BUILD.bazel` |
| `//src/backend/definitions:log_values` | `src/backend/definitions/BUILD.bazel` |
| `//src/backend/definitions:legacy_definition_columns` | `src/backend/definitions/BUILD.bazel` |
| `//src/backend/definitions:definitions` | `src/backend/definitions/BUILD.bazel` |
| `//src/backend/calibration:legacy_calibration_adapter` | `src/backend/calibration/BUILD.bazel` |
| `//src/backend/config:legacy_config_adapter` | `src/backend/config/BUILD.bazel` |
| `//src/backend/config:legacy_config_paths` | `src/backend/config/BUILD.bazel` |
| `//src/backend/definition:legacy_definition_adapter` | `src/backend/definition/BUILD.bazel` |
| `//src/backend/logging:legacy_logger_adapter` | `src/backend/logging/BUILD.bazel` |
| `//src/algorithms/protocol:qt_compat` | `src/algorithms/protocol/BUILD.bazel` |
| `//src/algorithms/expression:qt_compat` | `src/algorithms/expression/BUILD.bazel` |

`:definitions` depends on all twelve (the first five directly, the calibration/config/definition/logging adapters and both `:qt_compat` shims transitively). The design doc's completion criterion — "`//src/backend/definitions:definitions` does not depend on `@rules_qt//:qt_widgets`" — cannot hold while any one of these twelve keeps `QT_DEPS`. In every one of these seven `BUILD.bazel` files, **every** use of the `QT_DEPS` symbol is one of the twelve lines above (confirmed by grep — no other target in any of these seven files references `QT_DEPS`), so the fix is a straight symbol swap in each file's `load()` plus each matching `deps` line: no target keeps both bundles.

`//src/algorithms/expression:qt_compat` is deleted outright in 6a-4, not this slice, so Task 1 still converts it now — it must not carry widgets for the remainder of this slice's life, and converting it costs nothing extra since none of its code touches widgets either.

**`QT_DEPS_NO_WIDGETS` must also drop `@rules_qt//:qt_charts`, not only `@rules_qt//:qt_widgets`** — discovered while executing this task: `bazel cquery 'somepath(@rules_qt//:qt_charts, @rules_qt//:qt_widgets)'` shows `qt_charts` itself depends on `qt_widgets` (Qt Charts links QtWidgets), so leaving `qt_charts` in the bundle silently reopens the same edge the bundle exists to close. Every other `QT_DEPS` member (core, gui, remote_objects, serial_port, test, web_sockets, xml) was queried the same way and is clean. None of the twelve targets above reference any QtCharts symbol, so dropping it costs nothing for them.

**`//src/backend/definitions:definitions` itself is NOT one of the targets Task 1 converts**, despite appearing in the table above and despite being the ultimate target the design doc's criterion names. `file_actions.h` still `#include`s `<QApplication>`, `<QWidget>`, `<QScreen>`, `<QFileDialog>`, `<QMessageBox>`, `<QLabel>`, `<QComboBox>` **today** — Task 2 hasn't run yet — so `:definitions`'s own compile of `file_actions.cpp`/`file_actions_romraider.cpp`/`file_actions_ecuflash.cpp` genuinely still needs Qt Widgets headers on the include path. Moving `:definitions` onto `QT_DEPS_NO_WIDGETS` in Task 1 was tried and fails to compile with `fatal error: 'QApplication' file not found`. The other eleven targets in the table have no such dependency (none of them include `file_actions.h` or any widget header, confirmed by grep) and convert safely now. `:definitions`'s own `deps` line stays on full `QT_DEPS` through Task 1 and moves to `QT_DEPS_NO_WIDGETS` in **Task 2**, once its Step 1 removes those seven includes — see Task 2's new final step.

---

### Task 1: Introduce `QT_DEPS_NO_WIDGETS` and drop widgets from every non-widget Qt-typed target `:definitions` depends on (but not `:definitions` itself yet)

**Files:**
- Modify: `bazel/qt_targets.bzl`
- Modify: `src/backend/definitions/BUILD.bazel`
- Modify: `src/backend/calibration/BUILD.bazel`
- Modify: `src/backend/config/BUILD.bazel`
- Modify: `src/backend/definition/BUILD.bazel`
- Modify: `src/backend/logging/BUILD.bazel`
- Modify: `src/algorithms/protocol/BUILD.bazel`
- Modify: `src/algorithms/expression/BUILD.bazel`

**Interfaces:**
- Produces: `QT_DEPS_NO_WIDGETS` (a `list[Label]`) exported from `bazel/qt_targets.bzl`. Task 2 reads this symbol by name: its final step swaps `//src/backend/definitions:definitions`'s own `deps` line onto it once `file_actions.h` no longer needs Qt Widgets headers — that is the one target this task deliberately leaves on full `QT_DEPS`. 6a-4 and 6a-5 (out of scope here) will use the same symbol again when they touch `//src/algorithms/expression` and add `//:backend_no_widgets`.

This task is fully independent of Task 2 and Task 3: it changes no `.h`/`.cpp` file, only `deps` lists, and the resulting binaries are byte-identical modulo the dropped `-lQt6Widgets` link input. `bazel test --config=release //...` must already be green after this task alone.

- [ ] **Step 1: Add the constant**

In `bazel/qt_targets.bzl`, immediately after the closing `]` of `QT_DEPS` (currently line 20):

```python
QT_DEPS = [
    "@rules_qt//:qt_charts",
    "@rules_qt//:qt_core",
    "@rules_qt//:qt_gui",
    "@rules_qt//:qt_remote_objects",
    "@rules_qt//:qt_serial_port",
    "@rules_qt//:qt_test",
    "@rules_qt//:qt_web_sockets",
    "@rules_qt//:qt_widgets",
    "@rules_qt//:qt_xml",
]

# For qt_cc_library targets that are Qt-typed (QString/QStringList members,
# no QObject/Q_OBJECT of their own) but never construct or show a widget --
# e.g. the *_values.h data structs and the legacy_*_adapter Qt-compat
# shims. Keeping @rules_qt//:qt_widgets off these targets' own deps line is
# what lets //src/backend/definitions:definitions itself stay off it too,
# once file_actions.{h,cpp} stops using QWidget (step 6a-3).
#
# Also drops @rules_qt//:qt_charts: verified via
# `bazel cquery 'somepath(@rules_qt//:qt_charts, @rules_qt//:qt_widgets)'`
# that qt_charts itself depends on qt_widgets (Qt Charts links QtWidgets),
# so leaving it in QT_DEPS_NO_WIDGETS would silently reopen the same edge
# this constant exists to close. None of the twelve targets this constant
# is used for reference any QtCharts symbol (QChart/QLineSeries/etc. --
# confirmed by grep), so dropping it costs nothing. Every other QT_DEPS
# member (core, gui, remote_objects, serial_port, test, web_sockets, xml)
# was queried the same way and is clean.
QT_DEPS_NO_WIDGETS = [dep for dep in QT_DEPS if dep not in ("@rules_qt//:qt_widgets", "@rules_qt//:qt_charts")]
```

- [ ] **Step 2: Swap four of the five `src/backend/definitions` targets — not `definitions` itself**

In `src/backend/definitions/BUILD.bazel`, change the load line — this file still needs `QT_DEPS` too, since the `definitions` target keeps it, so load both:

```python
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "QT_DEPS_NO_WIDGETS", "fastecu_qttest", "qt_cc_library")
```

Then, for exactly `config_values`, `ecu_cal_def`, `log_values`, and `legacy_definition_columns` — **not** `definitions` — change `deps = QT_DEPS + [` to `deps = QT_DEPS_NO_WIDGETS + [`. Leave `definitions`'s own `deps = QT_DEPS + [...]` line untouched: `file_actions.h` still `#include`s `<QApplication>` and friends today, so this target still needs full `QT_DEPS` until Task 2 removes those includes. (`fastecu_qttest`'s four call sites in this file are also untouched — that macro supplies its own `QT_DEPS` internally for the *test* binary, unrelated to what the *library* target depends on.)

- [ ] **Step 3: Swap `legacy_calibration_adapter`**

In `src/backend/calibration/BUILD.bazel`, change the load line's `"QT_DEPS"` to `"QT_DEPS_NO_WIDGETS"`, and change `legacy_calibration_adapter`'s `deps = QT_DEPS + [` to `deps = QT_DEPS_NO_WIDGETS + [`.

- [ ] **Step 4: Swap `legacy_config_adapter` and `legacy_config_paths`**

In `src/backend/config/BUILD.bazel`, change the load line's `"QT_DEPS"` to `"QT_DEPS_NO_WIDGETS"`, and change both `legacy_config_adapter`'s and `legacy_config_paths`'s `deps = QT_DEPS + [` to `deps = QT_DEPS_NO_WIDGETS + [`.

- [ ] **Step 5: Swap `legacy_definition_adapter`**

In `src/backend/definition/BUILD.bazel`, change the load line's `"QT_DEPS"` to `"QT_DEPS_NO_WIDGETS"`, and change `legacy_definition_adapter`'s `deps = QT_DEPS + [` to `deps = QT_DEPS_NO_WIDGETS + [`.

- [ ] **Step 6: Swap `legacy_logger_adapter`**

In `src/backend/logging/BUILD.bazel`, change the load line's `"QT_DEPS"` to `"QT_DEPS_NO_WIDGETS"`, and change `legacy_logger_adapter`'s `deps = QT_DEPS + [` to `deps = QT_DEPS_NO_WIDGETS + [`.

- [ ] **Step 7: Swap both `qt_compat` shims**

In `src/algorithms/protocol/BUILD.bazel`, change the load line's `"QT_DEPS"` to `"QT_DEPS_NO_WIDGETS"`, and change `qt_compat`'s `deps = QT_DEPS + [":protocol"]` to `deps = QT_DEPS_NO_WIDGETS + [":protocol"]`.

In `src/algorithms/expression/BUILD.bazel`, change the load line's `"QT_DEPS"` to `"QT_DEPS_NO_WIDGETS"`, and change `qt_compat`'s `deps = QT_DEPS + [":expression"]` to `deps = QT_DEPS_NO_WIDGETS + [":expression"]`.

- [ ] **Step 8: Build and verify the four converted targets still resolve, and confirm the remaining path is exactly the expected one**

```bash
bazel build --config=release //src/backend/definitions:definitions
bazel cquery --config=release 'somepath(//src/backend/definitions:definitions, @rules_qt//:qt_widgets)'
```

Expected: the build succeeds (unchanged — `definitions` itself still has full `QT_DEPS`), and the `cquery` still shows a path — this task does not close it — but now the path is only the **direct** edge `//src/backend/definitions:definitions -> @rules_qt//:qt_widgets` from `definitions`'s own unmodified `deps` line, not a path through `config_values`/`ecu_cal_def`/`log_values`/`legacy_definition_columns` or through `qt_charts`. (Before this task, `bazel cquery --config=release 'somepath(//src/backend/definitions:config_values, @rules_qt//:qt_widgets)'` also showed a path; after this task's Step 2, that query shows none — spot-check it here too.) Closing the last, direct edge from `definitions` itself is Task 2's job, once `file_actions.h` stops needing Qt Widgets headers.

- [ ] **Step 9: Run the full test suite**

```bash
bazel test --config=release //...
```

Expected: PASS, identical to the pre-Task-1 baseline (this task changes no code, only link inputs).

- [ ] **Step 10: Commit**

```bash
prek run --all-files
git add bazel/qt_targets.bzl src/backend/definitions/BUILD.bazel src/backend/calibration/BUILD.bazel \
        src/backend/config/BUILD.bazel src/backend/definition/BUILD.bazel src/backend/logging/BUILD.bazel \
        src/algorithms/protocol/BUILD.bazel src/algorithms/expression/BUILD.bazel
git commit -m "$(cat <<'EOF'
build: add a widgets-free Qt deps bundle and drop qt_widgets from data-only targets

QT_DEPS always included @rules_qt//:qt_widgets and @rules_qt//:qt_charts
(which itself depends on qt_widgets), so every Qt-typed target that used
it -- four targets in src/backend/definitions and four "legacy adapter"
shims elsewhere in src/backend, plus two :qt_compat shims -- pulled
widgets into //src/backend/definitions:definitions's transitive closure
even though none of them touch a QWidget or QtCharts class.
QT_DEPS_NO_WIDGETS is QT_DEPS minus those two labels; this repoints
every one of those eleven onto it.

//src/backend/definitions:definitions itself is deliberately NOT
converted here: file_actions.h still #includes <QApplication> and
friends today, so its own compile still needs full QT_DEPS until step
6a-3's next task removes those includes and makes the same swap for
this one remaining target.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01LgogSKq3zbxLZTTELFXAuQ
EOF
)"
```

---

### Task 2: Convert `FileActions` to an injected `IEventSink`, and its own test suite

**Files:**
- Modify: `src/backend/definitions/file_actions.h`
- Modify: `src/backend/definitions/file_actions.cpp`
- Modify: `src/backend/definitions/file_actions_romraider.cpp`
- Modify: `src/backend/definitions/file_actions_ecuflash.cpp`
- Modify: `src/backend/definitions/file_actions_parsing_test.cpp`
- Modify: `src/backend/definitions/ecuflash_definition_parsing_test.cpp`
- Modify: `src/backend/definitions/rom_transformations_test.cpp`
- Modify: `src/backend/definitions/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::IEventSink`, `fastecu::LogLevel` from `src/backend/ports/event_sink.h` (already a `:definitions` dep via `//src/backend/ports`); `fastecu::RecordingEventSink` from `src/backend/ports/testing/recording_event_sink.h` (target `//src/backend/ports/testing:recording_event_sink`, not yet a dep of the three test targets touched here).
- Produces: `FileActions(fastecu::IFileSystem&, fastecu::IResourceBundle&, fastecu::IFileRepository&, fastecu::IAtomicFileWriter&, fastecu::IEventSink&)` — no default for the fifth parameter, no `QWidget` base, no `Q_OBJECT`, no signals. Task 3 (`MainWindow`/`Settings`) and the one already-updated call site in `//src/ui/desktop/definition:definition_authoring_dialog_test.cpp`'s test consume this signature.

This task's own package (`//src/backend/definitions/...`) must build and test green at the end of this task; `bazel build --config=release //:fastecu` is expected to **fail** until Task 3 fixes `mainwindow.cpp`/`settings.cpp` — both still call the old 4-argument constructor. Do not run `bazel test --config=release //...` as this task's success check; use the scoped command in Step 10.

- [ ] **Step 1: Update `file_actions.h`**

Replace the include block (lines 1-21) and class declaration (lines 43-51, 255-260):

```cpp
#pragma once

#include <QDomDocument>
#include <QXmlStreamReader>
#include <QDebug>
#include <QElapsedTimer>
#include <QDateTime>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/definitions/config_values.h"
#include "src/backend/definitions/ecu_cal_def.h"
#include "src/backend/definitions/log_values.h"
#include "src/backend/calibration/legacy_calibration_adapter.h"
#include "src/backend/config/legacy_config_adapter.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/definition/definition_service.h"
#include "src/backend/definition/legacy_definition_adapter.h"
#include "src/backend/ports/atomic_file_writer.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"
#include "src/backend/ports/resource_bundle.h"

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
#include <windows.h>
#else
#include <unistd.h>
#endif // Windows

class FileActions
{
  public:
    FileActions(fastecu::IFileSystem& file_system, fastecu::IResourceBundle& resource_bundle,
                fastecu::IFileRepository& file_repository, fastecu::IAtomicFileWriter& atomic_file_writer,
                fastecu::IEventSink& events);
```

(`QApplication`, `QWidget`, `QScreen`, `QFileDialog`, `QMessageBox`, `QLabel`, `QComboBox` are dropped — none of the widget classes they declare are referenced anywhere else in this header or the three `.cpp` files after this task's remaining steps. `QDomDocument`/`QXmlStreamReader` stay: `collect_ecuflash_base_header_fields` still parses with `QDomDocument`. `QDebug`/`QElapsedTimer`/`QDateTime` stay: nothing in this task touches their call sites.)

Everything between `float_precision` (line 52) and `parse_dtc_message` (line 204) is unchanged. Then replace the private section's end and the signals block (lines 249-260):

```cpp
    std::vector<std::string> submittedEcuflashHandles_;
    // Declared last so that definitionFileRepository_ -- its constructor
    // argument -- is already initialized: C++ initializes members in
    // declaration order regardless of initializer-list order.
    fastecu::calibration::LegacyCalibrationAdapter calibrationAdapter_;
    fastecu::IEventSink& events_;
};
```

(The `events_` member's own initialization order doesn't depend on any other member, so appending it after `calibrationAdapter_` — rather than disturbing that documented ordering constraint — is the minimal change. The `signals:` block and its four declarations are deleted outright.)

- [ ] **Step 2: Update `file_actions.cpp`**

Constructor (lines 135-144):

```cpp
FileActions::FileActions(fastecu::IFileSystem& file_system, fastecu::IResourceBundle& resource_bundle,
                         fastecu::IFileRepository& file_repository, fastecu::IAtomicFileWriter& atomic_file_writer,
                         fastecu::IEventSink& events)
    : configAdapter_(file_system, resource_bundle, file_repository), definitionFileSystem_(file_system),
      definitionFileRepository_(file_repository), loggerResourceBundle_(resource_bundle),
      loggerAtomicFileWriter_(atomic_file_writer),
      definitionService_(file_system, file_repository, atomic_file_writer), definitionAdapter_(definitionService_),
      calibrationAdapter_(file_repository), events_(events)
{
}
```

`log_definition_error` (lines 186-191):

```cpp
void FileActions::log_definition_error(const QString& operation, const fastecu::Error& error)
{
    events_.log(fastecu::LogLevel::Error, (operation + " [" + QString::fromUtf8(fastecu::to_string(error.kind)) +
                                           "]: " + QString::fromStdString(error.detail))
                                              .toStdString());
}
```

`log_definition_load_failure` (lines 235-246):

```cpp
bool FileActions::log_definition_load_failure(const QString& operation, const fastecu::Error& error,
                                              const QString& source, const QString& warning_title,
                                              const QString& warning_text)
{
    log_definition_error(operation, error);
    if (!source.isEmpty() && !definitionFileSystem_.exists(source.toStdString()))
    {
        events_.notice((warning_title + ": " + warning_text + source + " for reading").toStdString());
        return true;
    }
    return false;
}
```

`apply_flash_method_alias` (lines 317-318), replace both lines:

```cpp
            events_.log(fastecu::LogLevel::Debug, ("Alias: " + flashMethod).toStdString());
            events_.log(fastecu::LogLevel::Debug,
                       ("Protocol: " + ConfigValuesStruct.flash_protocol_protocol_name.at(index)).toStdString());
```

`read_logger_conf`: the `warnUnreadable` lambda (lines 591-595):

```cpp
    const auto warnUnreadable = [&]
    {
        events_.notice(
            ("Logger file: Unable to open logger config file '" + configValues->logger_file + "' for reading")
                .toStdString());
    };
```

Line 589 (`emit LOG_D("Looking for ECU ID..."`):

```cpp
    events_.log(fastecu::LogLevel::Debug,
               ("Looking for ECU ID: " + ecu_id + " in logger def file: " + configValues->logger_file).toStdString());
```

Line 638:

```cpp
        events_.log(fastecu::LogLevel::Debug, ("Found ECU ID " + ecu_id).toStdString());
```

Lines 647-650 (the `QMessageBox::warning` + following `emit LOG_D`, both fire on the same branch — convert each independently rather than merging them, so both the modal and the debug-log line legacy produced still happen):

```cpp
    if (logValues->log_value_protocol.empty())
    {
        events_.notice(
            "Logger definition file: No logger definition file selected, returning without initializing log "
            "parameters!");
        events_.log(fastecu::LogLevel::Debug,
                   "No logger definition file selected, returning without initializing log parameters!");
        return nullptr;
    }
```

Line 653:

```cpp
    events_.log(fastecu::LogLevel::Debug, "ECU ID not found, initializing log parameters");
```

Line 700-703 (`QMessageBox::warning` in `read_logger_definition_file`, "Unable to resolve..."):

```cpp
    if (!handle)
    {
        events_.notice(("Logger file: Unable to resolve logger definition file: " +
                        QString::fromStdString(handle.error().detail))
                           .toStdString());
        return logValues;
    }
```

Line 708:

```cpp
        events_.log(fastecu::LogLevel::Debug,
                   ("Using bundled CDBG logger definition: " + configValues->romraider_logger_definition_file)
                       .toStdString());
```

Line 715-718 (`QMessageBox::warning`, "Unable to open logger definition file..."):

```cpp
    if (!definition)
    {
        events_.notice(("Logger file: Unable to open logger definition file '" + QString::fromStdString(*handle) +
                        "' for reading: " + QString::fromStdString(definition.error().detail))
                           .toStdString());
        return logValues;
    }
```

Line 766:

```cpp
    events_.log(fastecu::LogLevel::Debug, ("EcuFlash cal id " + ecuCalDef->RomId + " found").toStdString());
```

Line 789:

```cpp
    events_.log(fastecu::LogLevel::Debug, ("RomRaider cal id " + ecuCalDef->RomId + " found").toStdString());
```

Line 822 (`QMessageBox::warning`, "Unable to open calibration file for reading" — `log_definition_error` on the line above is unchanged):

```cpp
        events_.notice("Calibration file: Unable to open calibration file for reading");
```

Line 926-929 (`emit LOG_W`):

```cpp
            events_.log(fastecu::LogLevel::Warning,
                       ("ROM size validation and map value decoding skipped: no resolved definition for id " +
                        ecuCalDef->RomId)
                           .toStdString());
```

Line 938 (`QMessageBox::warning`, "Error in expected ROM size!" — `log_definition_error` above is unchanged):

```cpp
                events_.notice("File size error: Error in expected ROM size!");
```

Line 956-957 (`QMessageBox::warning`, dead branch guarded by `ecuCalDef == nullptr` — preserved as-is, converted mechanically like every other site; this task does not remove dead code that isn't part of its own change):

```cpp
        events_.notice("Calibration file: Unable to find definition for selected calibration file with ECU ID: .");
```

Lines 974-975 (`emit LOG_E` + `QMessageBox::warning`, both fire together — convert both independently, same as the logger-conf pair above):

```cpp
    if (saved == nullptr)
    {
        // A failed write is the one failure in this file that must never be
        // silent: both callers in MainWindow historically ignored the return
        // value, so this notice is the user's only signal that the ROM they
        // are about to flash was not written.
        events_.log(fastecu::LogLevel::Error, ("Unable to open file " + filename + " for writing").toStdString());
        events_.notice(("Ecu calibration file: Unable to open file " + filename + " for writing").toStdString());
    }
```

- [ ] **Step 3: Update `file_actions_romraider.cpp`**

Line 12:

```cpp
        events_.log(fastecu::LogLevel::Debug, "No RomRaider definition files");
```

Line 20:

```cpp
        events_.log(fastecu::LogLevel::Debug, ("Reading RomRaider ID's from file: " + handle).toStdString());
```

Lines 32-35 (`QMessageBox::warning` inside the retry loop):

```cpp
                events_.notice(
                    ("Ecu definition file: Unable to open romraider definition file " + handle + " for reading")
                        .toStdString());
```

Lines 41, 43:

```cpp
    events_.log(fastecu::LogLevel::Debug,
               (QString::number(configValues->romraider_definition_files.size()) + " RomRaider definition files found")
                   .toStdString());
    events_.log(fastecu::LogLevel::Debug,
               (QString::number(configValues->romraider_def_cal_id.size()) + " RomRaider ecu id's found")
                   .toStdString());
```

Lines 87-89 (`QMessageBox::warning`, "Unable to open OEM ecu base definitions file..."):

```cpp
            events_.notice(
                ("Ecu definitions file: Unable to open OEM ecu base definitions file " + source + " for reading")
                    .toStdString());
```

Lines 111-114 (`QMessageBox::warning`, "No RomRaider definition file(s)..."):

```cpp
    if (ConfigValuesStruct.romraider_definition_files.isEmpty() &&
        ConfigValuesStruct.ecuflash_definition_files_directory.isEmpty())
    {
        events_.notice(
            "Ecu definition file: No RomRaider definition file(s), use definition manager at "
            "'Edit' menu to choose file(s)");
        return nullptr;
    }
```

Line 132 (the `tr("Ecu definitions file")` argument to `log_definition_load_failure`, inside `read_romraider_ecu_def`):

```cpp
        const bool missing =
            log_definition_load_failure("Unable to read RomRaider definition " + cal_id, replaced.error(), source,
                                        "Ecu definitions file", "Unable to open ECU definition file ");
```

Line 139:

```cpp
    events_.log(fastecu::LogLevel::Debug, ("XML ID: " + cal_id + " " + cal_id).toStdString());
```

- [ ] **Step 4: Update `file_actions_ecuflash.cpp`**

Line 11:

```cpp
        events_.log(fastecu::LogLevel::Debug, "No EcuFlash definition files directory");
```

Lines 29-30:

```cpp
    events_.log(fastecu::LogLevel::Debug, (QString::number(sources.size()) + " EcuFlash definition files found").toStdString());
    events_.log(fastecu::LogLevel::Debug,
               (QString::number(configValues->ecuflash_def_cal_id.size()) + " EcuFlash ecu id's found").toStdString());
```

Line 41:

```cpp
    events_.log(fastecu::LogLevel::Debug, ("Search ID: " + cal_id).toStdString());
```

Lines 48-49:

```cpp
    events_.log(fastecu::LogLevel::Debug, ("EcuFlash ID found: " + cal_id + " " + cal_id).toStdString());
    events_.log(fastecu::LogLevel::Debug, ("EcuFlash def file name: " + source).toStdString());
```

Line 57 (the `tr("Ecu definitions file")` argument, same helper as romraider's):

```cpp
        const bool missing =
            log_definition_load_failure("Unable to read EcuFlash definition " + cal_id, replaced.error(), source,
                                        "Ecu definitions file", "Unable to open ECU definition file ");
```

Lines 64-66:

```cpp
    events_.log(fastecu::LogLevel::Debug, ("Found ID: " + cal_id).toStdString());
    events_.log(fastecu::LogLevel::Debug,
               ("Definition for CAL ID " + cal_id + " succesfully read, start parsing definition scalings...")
                   .toStdString());
```

- [ ] **Step 5: Drop `:definitions` onto `QT_DEPS_NO_WIDGETS`, and build the library alone to catch internal mistakes early**

Steps 1-4 just removed the last `#include <QApplication>`/`<QWidget>`/`<QMessageBox>`/`<QFileDialog>`/`<QScreen>`/`<QLabel>`/`<QComboBox>` from this package (Task 1 deliberately left `//src/backend/definitions:definitions` itself on full `QT_DEPS`, exactly because those includes were still present — see Task 1's "Discovered prerequisite" section). Now that they're gone, make the swap Task 1 couldn't:

In `src/backend/definitions/BUILD.bazel`, the load line currently reads (after Task 1's Step 2):

```python
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "QT_DEPS_NO_WIDGETS", "fastecu_qttest", "qt_cc_library")
```

`definitions` is now the only target in this file still using the bare `QT_DEPS` symbol — change its `deps = QT_DEPS + [` to `deps = QT_DEPS_NO_WIDGETS + [`, then drop `"QT_DEPS"` from the load line (it has no remaining user in this file):

```python
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS_NO_WIDGETS", "fastecu_qttest", "qt_cc_library")
```

Then build and verify the widgets edge is finally gone end to end:

```bash
bazel build --config=release //src/backend/definitions:definitions
bazel cquery --config=release 'somepath(//src/backend/definitions:definitions, @rules_qt//:qt_widgets)'
```

Expected: the build succeeds, and the `cquery` prints nothing under `INFO: Found N targets...` besides the two header lines — no path exists at all now, closing the direct edge Task 1's Step 8 left in place on purpose. (This target has no test callers yet, so the build only proves the header/three `.cpp` files are internally consistent — it does not yet prove the tests or `MainWindow`/`Settings` compile.)

- [ ] **Step 6: Convert `file_actions_parsing_test.cpp`**

First, the mechanical bulk edit — every `FileActions actions(...)`/`FileActions fileActions(...)` construction in this file uses exactly the pattern `FileActions <var>(fileSystem_, resourceBundle_, fileRepository_, <writer-arg>);` where `<var>` is `actions` or `fileActions` and `<writer-arg>` is `atomicFileWriter_` or `writer`. Run:

```bash
perl -0777 -pi -e 's/^([ \t]*)FileActions (actions|fileActions)\(fileSystem_, resourceBundle_, fileRepository_, (atomicFileWriter_|writer)\);/$1fastecu::RecordingEventSink eventSink;\n$1FileActions $2(fileSystem_, resourceBundle_, fileRepository_, $3, eventSink);/gm' src/backend/definitions/file_actions_parsing_test.cpp
```

(`-0777` slurps the whole file into one string so the multi-line replacement can be inserted; the trailing `m` flag is what makes `^` anchor to each line start within that string rather than only the very start of the file — omitting it silently matches nothing.)

Verify it caught all 29 sites and left none behind:

```bash
grep -c ", eventSink);" src/backend/definitions/file_actions_parsing_test.cpp   # expect 29
grep -n "FileActions \(actions\|fileActions\)(fileSystem_" src/backend/definitions/file_actions_parsing_test.cpp   # expect no output
```

Next, delete the now-meaningless `QSignalSpy` declarations and the one `QVERIFY(errorSpy.isValid())` that follows one of them. Every one of the nine spies has the exact shape below (var name `errorSpy` or `debugSpy`, target `&actions` or `&fileActions`, signal `LOG_E` or `LOG_D`):

```bash
perl -0777 -pi -e 's/^[ \t]*QSignalSpy (errorSpy|debugSpy)\(&(actions|fileActions), &FileActions::LOG_[ED]\);\n//gm' src/backend/definitions/file_actions_parsing_test.cpp
perl -0777 -pi -e 's/^[ \t]*QVERIFY\(errorSpy\.isValid\(\)\);\n//gm' src/backend/definitions/file_actions_parsing_test.cpp
```

Replace the `spyContainsMessage` helper (lines 72-76) with the `RecordingEventSink`-based equivalents, and add the `#include`:

```cpp
#include "src/backend/ports/testing/recording_event_sink.h"
```

(add this to the include block, alphabetically after `"src/backend/definition/ecuflash_parser.h"` and before `"src/backend/definitions/file_actions.h"`)

```cpp
bool sinkContainsMessage(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level, const QString& text)
{
    return std::ranges::any_of(sink.logs, [&](const auto& entry)
                               { return entry.first == level && QString::fromStdString(entry.second).contains(text); });
}

int logCountAt(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level)
{
    return static_cast<int>(
        std::ranges::count_if(sink.logs, [level](const auto& entry) { return entry.first == level; }));
}
```

(this file has no `errorSpy.isEmpty()`-style call needing a "first message" helper — unlike Steps 7 and 8 below, don't add a `firstMessageAt` here; an unused anonymous-namespace function trips `-Wunused-function`)

(delete the old `spyContainsMessage` definition entirely — this file no longer uses `QSignalSpy` for `FileActions` at all)

Then replace every call site by hand (mechanical rename, but the argument shape differs slightly so this is not a single sed):

- `QVERIFY(spyContainsMessage(debugSpy, "...text..."));` → `QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Debug, "...text..."));` (3 occurrences, all following the debug spy at former line 457).
- `QVERIFY(spyContainsMessage(errorSpy, "...text..."));` → `QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "...text..."));` (occurrences following each former error spy: after `read_romraider_ecu_base_def`/`read_romraider_ecu_def`/`parse_ecuid_romraider_def_files`/`save_subaru_rom_file` assertions — grep for `spyContainsMessage(errorSpy` after the bulk edits above to find every remaining one).
- `QCOMPARE(errorSpy.count(), 0);` → `QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 0);`
- `QCOMPARE(errorSpy.count(), 1);` → `QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);` (multiple occurrences)

Verify none remain:

```bash
grep -n "errorSpy\|debugSpy\|spyContainsMessage" src/backend/definitions/file_actions_parsing_test.cpp
```

Expected: no output.

Now the six modal-dismissal blocks. Three are vestigial (the scenario they guard never reaches a `QMessageBox` even today) and are deleted outright with no replacement; three guard a real notice and get a `sinkContainsMessage`/count assertion in its place.

**Block 1 — `logger_conf_returns_nullptr_when_the_file_is_missing`** (guards the real `QMessageBox` inside `warnUnreadable`, now `events_.notice("Logger file: Unable to open logger config file '...' for reading")`):

```cpp
        QTimer::singleShot(0,
                           []()
                           {
                               if (QWidget *modal = QApplication::activeModalWidget())
                               {
                                   modal->close();
                               }
                           });
        QCOMPARE(actions.read_logger_conf(&values, "ECUID1", false), nullptr);
```
becomes
```cpp
        QCOMPARE(actions.read_logger_conf(&values, "ECUID1", false), nullptr);
        QVERIFY(!eventSink.notices.empty());
        QVERIFY(QString::fromStdString(eventSink.notices.front()).contains("Unable to open logger config file"));
```

(`events_.notice` lands in `RecordingEventSink::notices` — a `std::vector<std::string>` — not in `logs`, which only collects `events_.log(...)` calls.)

**Block 2 — `logger_conf_without_a_definition_warns_and_writes_nothing`** (guards `events_.notice("Logger definition file: No logger definition file selected...")`):

```cpp
        QTimer::singleShot(0,
                           []()
                           {
                               if (QWidget *modal = QApplication::activeModalWidget())
                               {
                                   modal->close();
                               }
                           });
        QCOMPARE(actions.read_logger_conf(&values, "ECUID1", false), nullptr);
```
becomes
```cpp
        QCOMPARE(actions.read_logger_conf(&values, "ECUID1", false), nullptr);
        QVERIFY(!eventSink.notices.empty());
        QVERIFY(QString::fromStdString(eventSink.notices.front())
                   .contains("No logger definition file selected"));
```

**Block 3 — `missing_romraider_base_returns_null_and_preserves_caller_state`** (guards `events_.notice("Ecu definitions file: Unable to open OEM ecu base definitions file...")`):

```cpp
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);
        QTimer::singleShot(0,
                           []()
                           {
                               if (QWidget *modal = QApplication::activeModalWidget())
                               {
                                   modal->close();
                               }
                           });

        QCOMPARE(actions.read_romraider_ecu_base_def(&ecu), nullptr);

        QCOMPARE(ecu.RomInfo, romInfo);
        QCOMPARE(ecu.DefinitionFileName, definitionFileName);
        QCOMPARE(ecu.NameList, names);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "cannot open file"));
```

(this one's `QSignalSpy`/`QTimer::singleShot` pair was already removed by the earlier bulk edits — this listing is here to identify the site, not to be applied verbatim) becomes:

```cpp
        QCOMPARE(actions.read_romraider_ecu_base_def(&ecu), nullptr);

        QCOMPARE(ecu.RomInfo, romInfo);
        QCOMPARE(ecu.DefinitionFileName, definitionFileName);
        QCOMPARE(ecu.NameList, names);
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "cannot open file"));
        QVERIFY(!eventSink.notices.empty());
        QVERIFY(QString::fromStdString(eventSink.notices.front())
                   .contains("Unable to open OEM ecu base definitions file"));
```

**Block 4 (vestigial) — `open_subaru_rom_file_grows_a_rom_that_matches_no_definition`**: with no romraider or EcuFlash definition source configured, `open_subaru_rom_file` takes neither definition branch, so `use_ecuflash_definition`/`use_romraider_definition` stay false and the `if (ecuCalDef->use_romraider_definition || ecuCalDef->use_ecuflash_definition)` guard around the only notice/log site in this function is never entered — no modal was ever reachable from this test even before this change. Delete the whole block and its now-stale comment:

```cpp
        // No definition resolves here, so open_subaru_rom_file shows its
        // "no definition found" chooser, which would block forever on an
        // offscreen QApplication. Closing it reports Rejected -- the
        // "continue without definition" path.
        QTimer modalCloser;
        modalCloser.setInterval(10);
        QObject::connect(&modalCloser, &QTimer::timeout,
                         []()
                         {
                             if (QWidget *modal = QApplication::activeModalWidget())
                             {
                                 modal->close();
                             }
                         });
        modalCloser.start();

        ecuCalDef = fileActions.open_subaru_rom_file(ecuCalDef, romPath);
        modalCloser.stop();
```
becomes
```cpp
        ecuCalDef = fileActions.open_subaru_rom_file(ecuCalDef, romPath);
```

**Block 5 — `open_subaru_rom_file_returns_nullptr_on_read_failure`** (guards `events_.notice("Calibration file: Unable to open calibration file for reading")`):

```cpp
        // The read failure still reports through QMessageBox::warning (that
        // dialog is FileActions's own, not the relocated chooser); dismiss
        // whatever modal appears rather than hang, matching this suite's
        // existing offscreen-QApplication convention.
        QTimer::singleShot(0,
                           []()
                           {
                               if (QWidget *modal = QApplication::activeModalWidget())
                               {
                                   modal->close();
                               }
                           });

        FileActions::EcuCalDefStructure *result =
            fileActions.open_subaru_rom_file(ecuCalDef, dir.filePath("missing.bin"));

        QVERIFY(result == nullptr);
```
becomes
```cpp
        FileActions::EcuCalDefStructure *result =
            fileActions.open_subaru_rom_file(ecuCalDef, dir.filePath("missing.bin"));

        QVERIFY(result == nullptr);
        QVERIFY(!eventSink.notices.empty());
        QVERIFY(QString::fromStdString(eventSink.notices.front())
                   .contains("Unable to open calibration file for reading"));
```

**Block 6 — `save_subaru_rom_file_warns_and_returns_nullptr_when_write_fails`** (the original assertion checked the `LOG_E` spy, i.e. the log channel, not the modal; `events_.log(LogLevel::Error, "Unable to open file ... for writing")` is what now carries that, alongside the `events_.notice(...)` the modal used to show — the `errorSpy`/`QVERIFY(errorSpy.isValid())` pair here was already removed by the earlier bulk edit, so only the timer and the final assertion change):

```cpp
        QTimer::singleShot(0,
                           []()
                           {
                               if (QWidget *modal = QApplication::activeModalWidget())
                               {
                                   modal->close();
                               }
                           });

        FileActions::EcuCalDefStructure *result = fileActions.save_subaru_rom_file(&ecuCalDef, romPath);

        QVERIFY(result == nullptr);
        QVERIFY(spyContainsMessage(errorSpy, "for writing"));
```
becomes
```cpp
        FileActions::EcuCalDefStructure *result = fileActions.save_subaru_rom_file(&ecuCalDef, romPath);

        QVERIFY(result == nullptr);
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "for writing"));
```

Finally, drop the now-unused Qt includes and downgrade the test's `main()`, since nothing in this file shows a widget anymore:

```cpp
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
```

(remove `#include <QApplication>`, `#include <QSignalSpy>`, `#include <QTimer>` from the top of the file)

```cpp
QTEST_APPLESS_MAIN(TestFileActionsParsing)

#include "file_actions_parsing_test.moc"
```

(was `QTEST_MAIN(TestFileActionsParsing)`)

- [ ] **Step 7: Convert `ecuflash_definition_parsing_test.cpp`**

Same shape, smaller. Bulk constructor edit (all 14 sites use `FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);`):

```bash
perl -0777 -pi -e 's/^([ \t]*)FileActions fileActions\(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_\);/$1fastecu::RecordingEventSink eventSink;\n$1FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);/gm' src/backend/definitions/ecuflash_definition_parsing_test.cpp
grep -c ", eventSink);" src/backend/definitions/ecuflash_definition_parsing_test.cpp   # expect 14
```

Delete the seven `QSignalSpy` declarations:

```bash
perl -0777 -pi -e 's/^[ \t]*QSignalSpy (errorSpy|debugSpy)\(&fileActions, &FileActions::LOG_[ED]\);\n//gm' src/backend/definitions/ecuflash_definition_parsing_test.cpp
```

Add the include and replace the `spyContainsMessage` helper (lines 47-51) with three helpers — `sinkContainsMessage` and `logCountAt` with the identical bodies used in Step 6, plus `firstMessageAt` (which Step 6 did not need but this file does, for the `errorSpy.isEmpty()` call below):

```cpp
#include "src/backend/ports/testing/recording_event_sink.h"
```

```cpp
bool sinkContainsMessage(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level, const QString& text)
{
    return std::ranges::any_of(sink.logs, [&](const auto& entry)
                               { return entry.first == level && QString::fromStdString(entry.second).contains(text); });
}

int logCountAt(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level)
{
    return static_cast<int>(
        std::ranges::count_if(sink.logs, [level](const auto& entry) { return entry.first == level; }));
}

QString firstMessageAt(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level)
{
    for (const auto& entry : sink.logs)
    {
        if (entry.first == level)
        {
            return QString::fromStdString(entry.second);
        }
    }
    return {};
}
```

Then convert each call site:

- `QVERIFY2(errorSpy.isEmpty(), errorSpy.isEmpty() ? "" : qPrintable(errorSpy.at(0).at(0).toString()));` (line 79) → `QVERIFY2(logCountAt(eventSink, fastecu::LogLevel::Error) == 0, qPrintable(firstMessageAt(eventSink, fastecu::LogLevel::Error)));`
- Every `QVERIFY(spyContainsMessage(debugSpy, "..."));` → `QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Debug, "..."));`
- Every `QVERIFY(spyContainsMessage(errorSpy, "..."));` → `QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "..."));`
- Every `QCOMPARE(errorSpy.count(), N);` → `QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), N);`

The one modal block, in `missing_ecuflash_definition_reports_definition_not_found` (guards `events_.notice(...)` reached through `log_definition_load_failure`, which also produces the `logCountAt(eventSink, LogLevel::Error) == 1` this test already asserts via `log_definition_error`):

```cpp
        QSignalSpy errorSpy(&fileActions, &FileActions::LOG_E);
        QTimer::singleShot(0,
                           []()
                           {
                               if (QWidget *modal = QApplication::activeModalWidget())
                               {
                                   modal->close();
                               }
                           });

        // Unlike read_romraider_ecu_base_def's single required base file, this rebuilds the
        // catalog from every currently known EcuFlash file (see build_definition_catalog), so
        // the unreadable entry is skipped rather than failing the whole lookup -- "MISSING" is
        // then simply absent from the resulting (empty) catalog. The configured source file
        // still doesn't exist on disk though, so this still takes the "missing file" branch
        // (a warning dialog, nullptr) rather than the "found but broken" branch.
        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "MISSING"), nullptr);

        QCOMPARE(ecuCalDef.RomInfo, romInfo);
        QCOMPARE(ecuCalDef.DefinitionFileName, definitionFileName);
        QCOMPARE(ecuCalDef.NameList, names);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "definition ID not found"));
        QVERIFY(spyContainsMessage(errorSpy, "MISSING"));
```
becomes (the `QSignalSpy` line is already gone from the earlier bulk delete; only the timer and the tail assertions change here):
```cpp
        // Unlike read_romraider_ecu_base_def's single required base file, this rebuilds the
        // catalog from every currently known EcuFlash file (see build_definition_catalog), so
        // the unreadable entry is skipped rather than failing the whole lookup -- "MISSING" is
        // then simply absent from the resulting (empty) catalog. The configured source file
        // still doesn't exist on disk though, so this still takes the "missing file" branch
        // (a notice, nullptr) rather than the "found but broken" branch.
        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "MISSING"), nullptr);

        QCOMPARE(ecuCalDef.RomInfo, romInfo);
        QCOMPARE(ecuCalDef.DefinitionFileName, definitionFileName);
        QCOMPARE(ecuCalDef.NameList, names);
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "definition ID not found"));
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "MISSING"));
        QVERIFY(!eventSink.notices.empty());
        QVERIFY(QString::fromStdString(eventSink.notices.front())
                   .contains("Unable to open ECU definition file"));
```

Drop the unused includes and downgrade `main()`:

```cpp
#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
```

(remove `#include <QApplication>`, `#include <QSignalSpy>`, `#include <QTimer>`)

```cpp
QTEST_APPLESS_MAIN(TestEcuflashDefinitionParsing)
#include "ecuflash_definition_parsing_test.moc"
```

- [ ] **Step 8: Convert `rom_transformations_test.cpp`**

Single construction site:

```cpp
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
```
becomes
```cpp
        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
```

Add the include:

```cpp
#include "src/backend/ports/testing/recording_event_sink.h"
```

The one `QSignalSpy` plus its two `QVERIFY2` uses and the one (vestigial — see Task 2 Step 6 Block 4's reasoning: this scenario has a resolving definition and a matching ROM size, so `open_subaru_rom_file`'s only notice site is never reached) modal block:

```cpp
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);

        FileActions::EcuCalDefStructure parsed;
        QCOMPARE(actions.read_romraider_ecu_def(&parsed, "CAL1"), &parsed);
        QVERIFY2(errorSpy.isEmpty(), errorSpy.isEmpty() ? "" : qPrintable(errorSpy.at(0).at(0).toString()));
        QTimer::singleShot(0,
                           []()
                           {
                               if (QWidget *modal = QApplication::activeModalWidget())
                               {
                                   modal->close();
                               }
                           });

        FileActions::EcuCalDefStructure ecu;
        QCOMPARE(actions.open_subaru_rom_file(&ecu, romPath), &ecu);
        QVERIFY2(errorSpy.isEmpty(), errorSpy.isEmpty() ? "" : qPrintable(errorSpy.at(0).at(0).toString()));
```
becomes
```cpp
        FileActions::EcuCalDefStructure parsed;
        QCOMPARE(actions.read_romraider_ecu_def(&parsed, "CAL1"), &parsed);
        QVERIFY2(logCountAt(eventSink, fastecu::LogLevel::Error) == 0,
                 qPrintable(firstMessageAt(eventSink, fastecu::LogLevel::Error)));

        FileActions::EcuCalDefStructure ecu;
        QCOMPARE(actions.open_subaru_rom_file(&ecu, romPath), &ecu);
        QVERIFY2(logCountAt(eventSink, fastecu::LogLevel::Error) == 0,
                 qPrintable(firstMessageAt(eventSink, fastecu::LogLevel::Error)));
```

This needs the same `logCountAt`/`firstMessageAt` helpers as Steps 6-7 (this file had no `spyContainsMessage` helper before, since it never used one). Do not also add `sinkContainsMessage` here: this file's only remaining assertions are the two `QVERIFY2` calls above, and an unused `static`/anonymous-namespace function trips `-Wunused-function`. Add just these two, to the anonymous namespace after `writeBinaryFile`:

```cpp
int logCountAt(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level)
{
    return static_cast<int>(
        std::ranges::count_if(sink.logs, [level](const auto& entry) { return entry.first == level; }));
}

QString firstMessageAt(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level)
{
    for (const auto& entry : sink.logs)
    {
        if (entry.first == level)
        {
            return QString::fromStdString(entry.second);
        }
    }
    return {};
}
```

Drop the unused includes and downgrade `main()`:

```cpp
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
```

(remove `#include <QApplication>`, `#include <QSignalSpy>`, `#include <QTimer>`)

```cpp
QTEST_APPLESS_MAIN(TestRomTransformations)

#include "rom_transformations_test.moc"
```

- [ ] **Step 9: Update `src/backend/definitions/BUILD.bazel`'s three affected test targets**

For `test_ecuflash_definition_parsing`, `test_file_actions_parsing`, and `test_rom_transformations`, remove `copts = ["-DQT_WIDGETS_LIB"]` and `env = {"QT_QPA_PLATFORM": "offscreen"}`, and add `"//src/backend/ports/testing:recording_event_sink"` to each `deps` list. For example, `test_file_actions_parsing` goes from:

```python
fastecu_qttest(
    name = "test_file_actions_parsing",
    src = "file_actions_parsing_test.cpp",
    copts = ["-DQT_WIDGETS_LIB"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    deps = [
        ":definitions",
        "//src/backend/definition:ecuflash_parser",
        "//src/backend/ports/testing:in_memory_atomic_file_writer",
        "//src/platform/desktop/common/ports",
    ],
)
```
to
```python
fastecu_qttest(
    name = "test_file_actions_parsing",
    src = "file_actions_parsing_test.cpp",
    deps = [
        ":definitions",
        "//src/backend/definition:ecuflash_parser",
        "//src/backend/ports/testing:in_memory_atomic_file_writer",
        "//src/backend/ports/testing:recording_event_sink",
        "//src/platform/desktop/common/ports",
    ],
)
```

Apply the same three changes (drop `copts`, drop `env`, add the `recording_event_sink` dep) to `test_ecuflash_definition_parsing` and `test_rom_transformations`. `test_model_validation` is untouched — it never constructed `FileActions` and never had these flags.

- [ ] **Step 10: Run the package-scoped test suite**

```bash
bazel test --config=release //src/backend/definitions/...
```

Expected: PASS, including all four `fastecu_qttest` targets. `bazel build --config=release //:fastecu` is still expected to fail at this point (Task 3 fixes it) — do not attempt it as a checkpoint here.

- [ ] **Step 11: Commit**

```bash
prek run --all-files
git add src/backend/definitions/
git commit -m "$(cat <<'EOF'
refactor: inject IEventSink into FileActions, drop QWidget/Q_OBJECT

FileActions stops inheriting QWidget and drops Q_OBJECT and its four
LOG_E/W/I/D signals; a fastecu::IEventSink& takes the place of the
trailing QWidget *parent constructor argument. The 25 emit LOG_*
statements become events_.log(...); the 12 QMessageBox::warning sites
become events_.notice(...), with the dialog's title folded into the
notice body as a prefix since IEventSink::notice carries one string.
All 13 tr() calls go with them -- the project installs no QTranslator,
so nothing here was ever actually translated at runtime.

The three affected test suites swap QSignalSpy(FileActions::LOG_*) and
QApplication::activeModalWidget()-based modal dismissal for a
fastecu::RecordingEventSink passed into the constructor, and downgrade
from QTEST_MAIN to QTEST_APPLESS_MAIN now that nothing under test shows
a widget. Two vestigial modal-dismissal blocks, whose test scenarios
never actually reached a QMessageBox even before this change, are
deleted outright rather than converted.

//src/backend/definitions:definitions itself now drops onto
QT_DEPS_NO_WIDGETS too -- the one target the prior QT_DEPS_NO_WIDGETS
commit deliberately left on full QT_DEPS, because this file still
needed Qt Widgets headers until now.

MainWindow and Settings, the two remaining callers of the old
constructor signature, are updated in the next commit -- //:fastecu
does not build between this commit and that one.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01LgogSKq3zbxLZTTELFXAuQ
EOF
)"
```

---

### Task 3: Rewire `MainWindow` and `Settings`, verify the full tree

**Files:**
- Modify: `src/ui/desktop/mainwindow.h`
- Modify: `src/ui/desktop/mainwindow.cpp`
- Modify: `src/ui/desktop/settings.h`
- Modify: `src/ui/desktop/settings.cpp`
- Modify: `src/ui/desktop/definition/definition_authoring_dialog_test.cpp`
- Modify: `src/ui/desktop/BUILD.bazel`

**Interfaces:**
- Consumes: `FileActions(IFileSystem&, IResourceBundle&, IFileRepository&, IAtomicFileWriter&, fastecu::IEventSink&)` from Task 2; `QtEventSink` (`logged(int, QString)`, `noticed(QString)`) from `src/platform/desktop/common/ports/qt_event_sink.h` (already a direct dep of `//src/ui/desktop`); `fastecu::NullEventSink` from `src/backend/ports/event_sink.h`.
- Produces: nothing further downstream — this closes the seam this slice opened. `fileActions` changes from `FileActions *` to `std::unique_ptr<FileActions>`; every existing `fileActions->` call site in `mainwindow.cpp` is unaffected by that change (`operator->` behaves identically), confirmed by grep: the only assignment site is the one this task edits, and there is no `delete fileActions` anywhere in the file to remove.

- [ ] **Step 1: Update `mainwindow.h`**

Add an include, right after the existing `"src/ui/desktop/definition/definition_authoring_dialog.h"` (line 49):

```cpp
#include "src/platform/desktop/common/ports/qt_event_sink.h"
```

Change the member declaration (line 193):

```cpp
    FileActions *fileActions;
```
to
```cpp
    QtEventSink fileActionsEvents_{this};
    std::unique_ptr<FileActions> fileActions;
```

(`fileActionsEvents_` is declared immediately before `fileActions` so it is constructed first — members initialize in declaration order regardless of where `fileActions` is later assigned in the constructor body — guaranteeing it outlives the `IEventSink&` reference `FileActions` will hold onto it.)

- [ ] **Step 2: Update `mainwindow.cpp`'s construction and wiring**

Replace the construction (lines 64-65):

```cpp
    fileActions =
        new FileActions(m_configFileSystem, m_configResourceBundle, m_configFileRepository, m_definitionFileWriter);
```
with
```cpp
    fileActions = std::make_unique<FileActions>(m_configFileSystem, m_configResourceBundle, m_configFileRepository,
                                                m_definitionFileWriter, fileActionsEvents_);
```

Replace the four direct connects (lines 107-110):

```cpp
    QObject::connect(fileActions, &FileActions::LOG_E, syslogger, &SystemLogger::log_messages);
    QObject::connect(fileActions, &FileActions::LOG_W, syslogger, &SystemLogger::log_messages);
    QObject::connect(fileActions, &FileActions::LOG_I, syslogger, &SystemLogger::log_messages);
    QObject::connect(fileActions, &FileActions::LOG_D, syslogger, &SystemLogger::log_messages);
```
with
```cpp
    QObject::connect(&fileActionsEvents_, &QtEventSink::logged, this,
                     [this](int level, QString message)
                     {
                         switch (static_cast<fastecu::LogLevel>(level))
                         {
                             case fastecu::LogLevel::Error:
                                 emit LOG_E(message, true, true);
                                 break;
                             case fastecu::LogLevel::Warning:
                                 emit LOG_W(message, true, true);
                                 break;
                             case fastecu::LogLevel::Info:
                                 emit LOG_I(message, true, true);
                                 break;
                             case fastecu::LogLevel::Debug:
                                 emit LOG_D(message, true, true);
                                 break;
                         }
                     });
    QObject::connect(&fileActionsEvents_, &QtEventSink::noticed, this,
                     [this](QString message) { QMessageBox::warning(this, software_title, message); });
```

(this routes through `MainWindow`'s own `LOG_E/W/I/D` signals — already connected to `syslogger` two lines above this block — rather than connecting `fileActionsEvents_` to `syslogger` directly, matching the established `FlashDialog`/`ServiceFunctionDialog` pattern at `src/ui/desktop/flash/common/flash_dialog.cpp:91-108` of re-emitting a leveled `QtEventSink::logged` through the owning widget's own leveled signals. `software_title` is already assigned a few lines above this point in the constructor and is reused here rather than a new hardcoded string, so the dialog title tracks the app's branding the same way every other `QMessageBox::warning(this, tr("..."), ...)` call in this file already does with a literal — the difference is this one no longer has a *specific* per-message title to show, since `IEventSink::notice` carries a single string with the original title already folded into its body by Task 2.)

The `DefinitionAuthoringDialog` construction right after (previously `new fastecu::ui::DefinitionAuthoringDialog(*fileActions, m_configFileRepository, this);`) is unchanged — `*fileActions` dereferences a `std::unique_ptr<FileActions>` exactly like it dereferenced the old raw pointer.

- [ ] **Step 3: Update `settings.h`**

Add an include after `"src/platform/desktop/common/ports/qt_resource_bundle.h"`:

```cpp
#include "src/backend/ports/event_sink.h"
```

Add a member near the other injected-port members (find the class's private section holding `m_configFileSystem`/etc. — add alongside them):

```cpp
    fastecu::NullEventSink m_fileActionsEvents;
```

(`Settings::save_config_file()` only calls `FileActions::save_config_file`, which delegates straight to `LegacyConfigAdapter::save_config_file` — a pure write with no notice/log call on any path. A `NullEventSink` is therefore behavior-preserving here, not a placeholder: there was never a modal or log line on this call path to lose.)

- [ ] **Step 4: Update `settings.cpp`**

Replace the construction in `Settings::save_config_file()`:

```cpp
    fileActions =
        new FileActions(m_configFileSystem, m_configResourceBundle, m_configFileRepository, m_definitionFileWriter);
```
with
```cpp
    fileActions = new FileActions(m_configFileSystem, m_configResourceBundle, m_configFileRepository,
                                  m_definitionFileWriter, m_fileActionsEvents);
```

(`Settings::fileActions` stays a raw `FileActions *`, never deleted, exactly as it is today — that pre-existing leak is untouched by this task; only the constructor call gains the fifth argument.)

- [ ] **Step 5: Add the Bazel dependency for `event_sink.h`**

In `src/ui/desktop/BUILD.bazel`, add `"//src/backend/ports"` to the `deps` of the target compiling `mainwindow.cpp`/`settings.cpp` (the one already listing `"//src/backend/definitions"` and `"//src/platform/desktop/common/ports"`) if it is not already present via a transitive-but-undeclared path. Check first:

```bash
grep -n '"//src/backend/ports"' src/ui/desktop/BUILD.bazel
```

If absent, add it next to the existing `"//src/backend/definitions"` line.

- [ ] **Step 6: Update `definition_authoring_dialog_test.cpp`'s construction call**

In `src/ui/desktop/definition/definition_authoring_dialog_test.cpp`, this file already constructs a real `FileActions` for its one test; it needs a fifth argument too. Add the include and a sink:

```cpp
#include "src/backend/ports/event_sink.h"
```

```cpp
    FileActions file_actions(file_system, resource_bundle, config_repository, writer);
```
becomes
```cpp
    fastecu::NullEventSink events;
    FileActions file_actions(file_system, resource_bundle, config_repository, writer, events);
```

(a `NullEventSink` is enough here too: this test only checks that `DefinitionAuthoringDialog` itself constructs and exposes its own four log signals — it never exercises a `FileActions` code path that would call into the sink.)

- [ ] **Step 7: Build the app**

```bash
bazel build --config=release //:fastecu
```

Expected: SUCCESS.

- [ ] **Step 8: Run the full test suite**

```bash
bazel test --config=release //...
```

Expected: PASS across every target, including `//:portable_closure`, `//:serial_compat_allowlist`, and `//:openpty_includes` (all unaffected by this slice), with the usual Windows-only skips on macOS.

- [ ] **Step 9: Verify the parallel-branch constraint**

```bash
git diff --name-only origin/master | grep -E 'src/platform/desktop/common/flash/legacy/|src/ui/desktop/flash/' || echo "OK: no drain-owned files touched"
```

Expected: `OK: no drain-owned files touched`.

- [ ] **Step 10: Commit**

```bash
prek run --all-files
git add src/ui/desktop/mainwindow.h src/ui/desktop/mainwindow.cpp src/ui/desktop/settings.h \
        src/ui/desktop/settings.cpp src/ui/desktop/definition/definition_authoring_dialog_test.cpp \
        src/ui/desktop/BUILD.bazel
git commit -m "$(cat <<'EOF'
refactor: rewire MainWindow and Settings onto FileActions's new IEventSink

FileActions no longer takes a QWidget *parent (step 6a-3's previous
commit), so MainWindow now owns it in a std::unique_ptr and constructs a
QtEventSink member ahead of it, wired the same way flash_dialog.cpp and
service_function_dialog.cpp already re-emit a leveled QtEventSink::logged
through their owner's own LOG_E/W/I/D signals -- and a new noticed
connection shows the same modal warning the user saw before, still
titled with the app's own name since the per-message title FileActions
used to pass now lives inside the notice body instead.

Settings' one FileActions construction (inside save_config_file, which
never notices or logs on any path) gets a NullEventSink; the
DefinitionAuthoringDialog construction test gets the same for the same
reason.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01LgogSKq3zbxLZTTELFXAuQ
EOF
)"
```

---

## Verification

After Task 3, these all hold — the exact "Completion criteria" from the design doc that this slice is responsible for (`//:backend_no_widgets` itself is 6a-5's job, not this slice's, but everything the guard will check is already true once this plan is done):

- `grep -rE 'QMessageBox|QFileDialog|QDialog|QWidget|Q_OBJECT' src/backend/definitions/file_actions.h src/backend/definitions/file_actions.cpp src/backend/definitions/file_actions_romraider.cpp src/backend/definitions/file_actions_ecuflash.cpp` matches nothing (not even a comment — none of the retained comments in this plan's edits mention those tokens).
- `bazel cquery --config=release 'somepath(//src/backend/definitions:definitions, @rules_qt//:qt_widgets)'` finds no path.
- `//src/backend/definitions:definitions`'s four `fastecu_qttest` targets no longer set `-DQT_WIDGETS_LIB` or `QT_QPA_PLATFORM=offscreen` (`test_model_validation` never did; the other three had both removed in Task 2 Step 9).
- `class FileActions` declares no base class and no `Q_OBJECT`.
- `grep -n "tr(" src/backend/definitions/file_actions.cpp src/backend/definitions/file_actions_romraider.cpp src/backend/definitions/file_actions_ecuflash.cpp` returns nothing.
- `bazel test --config=release //...` is green.
- `git diff --name-only origin/master` lists no path under `src/platform/desktop/common/flash/legacy/` or `src/ui/desktop/flash/`.
- Modality is preserved end to end: every `QMessageBox::warning` the user saw from `FileActions` before this slice, they still see — now via `MainWindow`'s `QtEventSink::noticed` → `QMessageBox::warning` connection — with the same message text (title folded in as a prefix) and the same trigger conditions.

## Deferred to 6a-4 / 6a-5

- `//src/algorithms/expression:qt_compat` still exists after this plan (Task 1 only stopped it from carrying widgets); its deletion, and draining `menu_actions.cpp` onto `ExpressionEvaluator` directly, is 6a-4.
- `//:backend_no_widgets` (the `py_test` guard in the shape of `scripts/check-serial-compat-allowlist.py`) and the modularization-plan/tech-debt-roadmap status updates are 6a-5.
