# Zero Warnings and Warnings-as-Errors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring first-party code to zero compiler and linker warnings at today's warning levels on every gated CI toolchain, then make any new warning fail the build.

**Architecture:** Two stacked pull requests. The first fixes every warning at its cause (no suppressions), grouped by root cause into one commit per task. The second adds `REPO.bazel` with `repo(features = ["treat_warnings_as_errors"])`, gives the hand-written x86 MSVC toolchain the same feature, keeps clang-tidy from double-gating compiler warnings, and records the decision in ADR 0018 and CLAUDE.md.

**Tech Stack:** Bazel 9.1.1 (`.bazelversion`), C++23, Qt 6.8.3, GoogleTest/gMock, Apple clang (macOS), GCC 15 (Linux CI), MSVC (Windows CI), Docker `gcc:15` for local GCC reproduction, `gh` with the `gh-stack` extension.

**Spec:** [docs/superpowers/specs/2026-10-03-zero-warnings-design.md](../specs/2026-10-03-zero-warnings-design.md)

## Global Constraints

- Scope is today's warning levels: toolchain defaults plus `COMMON_COPTS` (`-Wall` on clang/GCC, MSVC with no `/W` flag). Do not add or raise any warning flag; raising levels is issue #470.
- Fix every warning at its cause. No `#pragma` diagnostic suppressions, no `-Wno-*` flags, no `-Wno-error=`, and no `features = ["-treat_warnings_as_errors"]` anywhere in committed files.
- Every edit preserves behavior. In `J2534_unix.cpp` and `serial_port_actions_direct.cpp` the only permitted change is deleting dead code.
- An intentionally discarded result is written `std::ignore = call;` with `#include <tuple>`. `(void)name;` / `static_cast<void>(name);` for an unused variable or parameter stays as it is.
- `Result`/`Status` are checked with `.has_value()`; in tests use `fastecu::testing::IsOk()` from `src/backend/ports/testing/result_matchers.h`.
- Line numbers below were measured on master `5a0420c5`; locate each site by its quoted text, not its line number.
- The Android cross-compile (portable-core job) is not gated: `rules_android_ndk` 0.1.5 does not define `treat_warnings_as_errors`, and nothing is added to force it.
- PR 1 lives on branch `fix/clear-warnings` (already holds the spec commit). PR 2 lives on `build/warnings-as-errors`, branched from PR 1. Build them as a `gh stack`. Push and open PRs only once the user authorizes it.
- After editing any BUILD file run `python3 scripts/gazelle_check.py --fix` and commit its output. `prek` runs on every commit; do not bypass it.
- Every commit message ends with these two lines:

  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J
  ```

## Review Focus

1. Toggling a calibration map's On/Off checkbox must keep emitting `CalibrationMaps::checkbox_state_changed` with Qt's integer check state (2 checked, 0 unchecked); `MainWindow::set_map_switch` interprets that integer. Task 2 adds assertions on the emitted values.
2. Every macOS binary that links Qt must still find the frameworks at runtime after the rpath patch, with the rpath on each link exactly once. Task 9 counts `-rpath` in the link command and runs the Qt-linked tests.
3. External repositories (googletest, pugixml) must not be compiled with `-Werror`, while first-party sources must be. Task 11 checks both compile command lines with `bazel aquery`.
4. The J2534 bridge binaries built by the x86 MSVC toolchain must also fail on a warning. Task 14's negative control puts one probe in a source that the x86 toolchain compiles.
5. clang-tidy must stop failing on compiler warnings that only its newer LLVM reports, while still failing on its own checks. Task 12 adds a runner test asserting `-extra-arg=-Wno-error` on every platform; the existing tests keep covering failure on findings.

---

## Shared tooling

Two checks are used throughout. Neither is committed.

**Clean macOS build.** Bazel replays cached outputs without their diagnostics, so a build only shows warnings for actions it actually runs. To see every warning, build in a throwaway output base:

```bash
SCRATCH=$(mktemp -d)
bazel --output_base="$SCRATCH/ob" build -k --config=release --color=no --curses=no //... 2>&1 \
    | tee "$SCRATCH/build.log" | grep -E "warning:|warning: duplicate -rpath" | sort | uniq -c
bazel --output_base="$SCRATCH/ob" shutdown
```

Running Bazel with `--output_base` from the workspace repoints the `bazel-*` convenience symlinks at the scratch output base; the next ordinary `bazel build` puts them back. Run one before using `bazel-bin` paths again.

For a quick check after editing, an ordinary `bazel build --config=release //... 2>&1 | grep "warning:"` shows warnings only for the files that recompiled, which are exactly the edited ones and their dependents.

**GCC 15 in Docker.** Reproduces the Linux CI job's GCC warnings on macOS. Save as `$TMPDIR/gcc15_check.sh`, `chmod +x` it, and run it from the repository root after one `bazel build --config=release //...` (it reads generated headers from `bazel-bin`). With no arguments it checks every non-Windows first-party `.cpp`, which takes about ten minutes; with file arguments it checks only those.

```sh
#!/bin/sh
# Reproduce the Linux CI job's GCC 15 warnings on a macOS host, in Docker.
# Compiles first-party translation units at -O2 -Wall (the release config's
# flags) against the googletest, pugixml, and Linux Qt headers Bazel already
# fetched, plus the generated headers (uic, replicas, virtual includes) of
# the last `bazel build --config=release //...`. The repository is also
# mounted at its host path because Bazel's _virtual_includes headers are
# absolute symlinks into it.
#
# Usage: gcc15_check.sh [file.cpp ...]   (default: every non-Windows
#        first-party .cpp). Prints "== file" per translation unit, then its
#        warnings. A "fatal error" line means the file could not be checked
#        here; CI covers it.
set -eu
repo=$(git rev-parse --show-toplevel)
cd "$repo"
real() { python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1"; }
ob=$(bazel info output_base 2>/dev/null)
bin=$(real "$(bazel info --config=release bazel-bin 2>/dev/null)")
gtest=$(real "$ob/external/googletest+")
pugixml=$(real "$ob/external/pugixml+")
qt=$(real "$ob/external/rules_qt++fetch+qt_linux_x86_64")
generated=$(cd "$bin" && {
    find apps src tests -type d -name generated_ui
    find apps src tests -type d -path '*/_virtual_includes/*' -prune
    find apps src tests -name 'rep_*_replica.h' -exec dirname {} \;
} 2>/dev/null | sort -u | sed 's#^#-I/genbin/#' | tr '\n' ' ')
list=$(mktemp "${TMPDIR:-/tmp}/gcc15_check.XXXXXX")
if [ "$#" -gt 0 ]; then
    printf '%s\n' "$@" > "$list"
else
    git ls-files 'apps/*.cpp' 'resources/*.cpp' 'src/*.cpp' 'tests/*.cpp' \
        | grep -v -E '/windows/|_win(dows)?[_.]' > "$list"
fi
docker run --rm --platform linux/arm64 -e GENERATED="$generated" \
    -v "$repo":/src:ro -v "$repo":"$repo":ro -v "$bin":/genbin:ro -v "$gtest":/gtest:ro \
    -v "$pugixml":/pugixml:ro -v "$qt":/qt:ro -v "$list":/list:ro -w /src gcc:15 sh -c '
    xargs -P "$(nproc)" -I{} sh -c "
        out=\$(g++ -std=c++23 -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=1 -fPIC -Wall -Wunused-but-set-parameter -Wno-free-nonheap-object \
            -DQT_FORCE_ASSERTS -I. \$GENERATED \
            -I/gtest/googletest/include -I/gtest/googlemock/include -I/pugixml \
            -I/qt/include -I/qt/include/QtCore -I/qt/include/QtGui -I/qt/include/QtWidgets \
            -I/qt/include/QtSerialPort -I/qt/include/QtXml -I/qt/include/QtCharts \
            -I/qt/include/QtRemoteObjects -I/qt/include/QtWebSockets \
            -c {} -o /dev/null 2>&1 \
            | grep -E \"warning:|fatal error:|required from here|inlined from .* at (apps|resources|src|tests)/\" || true)
        printf \"== %s\n%s\n\" {} \"\$out\"
    " < /list'
rm -f "$list"
```

Summarize a run with:

```bash
$TMPDIR/gcc15_check.sh > /tmp/gcc.log 2>&1
grep -oE "\[-W[^]]+\]" /tmp/gcc.log | sort | uniq -c          # by flag
grep -E "required from here" /tmp/gcc.log | grep -E "^(\./)?(apps|src|tests)/"   # gtest instantiation sites
```

GCC reports a warning inside a template (for example googletest's `CmpHelperEQ`) once per type pair per translation unit, at the first call that instantiates it. Fixing that call moves the report to the next call with the same operand types. That is why Task 3 sweeps whole files and re-runs the check until it is silent.

---

## PR 1: `fix/clear-warnings`

### Task 1: Delete unused and set-but-unused locals

**Files:**
- Modify: `src/platform/desktop/unix/j2534/driver/J2534_unix.cpp` (`PassThruStartPeriodicMsg`, `PassThruReadVersion`, `PassThruIoctl`)
- Modify: `src/platform/desktop/common/serial/direct/common/serial_port_actions_direct.cpp` (`append_iso9141_header`, `get_serial_num`)
- Modify: `src/ui/desktop/widgets/mainwindow.cpp` (`calibration_data_treewidget_item_selected`, `add_new_logger_definition_file`, `remove_logger_definition_file`)
- Modify: `src/ui/desktop/widgets/definition_file_convert.cpp`
- Modify: `tests/tst_mut_dma_integration.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: no interface changes.

These are compiler-proven dead stores; existing tests cover the surrounding behavior.

- [ ] **Step 1: Record the warnings this task removes**

Run: `bazel build --config=release //... 2>&1 | grep -E "Wunused-variable|Wunused-but-set-variable"`
If nothing prints because the actions are cached, use the clean macOS build from Shared tooling. Expect the sites listed in Steps 2–6.

- [ ] **Step 2: `J2534_unix.cpp`**

In `J2534::PassThruStartPeriodicMsg`, delete these three declarations and the blank line after them:

```cpp
    PASSTHRU_MSG rxmsg;
    unsigned long numRxMsg;
    unsigned long timeout = 10;
```

In `J2534::PassThruReadVersion`, delete:

```cpp
    const char *fw_version = "Main code version: 1.17.4877";
```

In `J2534::PassThruIoctl`, delete:

```cpp
    SCONFIG *cfgitem;
```

- [ ] **Step 3: `serial_port_actions_direct.cpp`**

In `SerialPortActionsDirect::append_iso9141_header`, delete:

```cpp
    uint8_t msglength = output.length();
```

In `SerialPortActionsDirect::get_serial_num`, `inbuf` was only used by a call that is commented out. Replace the whole function with:

```cpp
bool SerialPortActionsDirect::get_serial_num(char *serial_arg)
{
    struct
    {
        unsigned int length;
        std::array<unsigned char, 256> data;
    } outbuf{};

    outbuf.length = outbuf.data.size() - 1; // reserve one byte for the null terminator
    memcpy(serial_arg, outbuf.data.data(), outbuf.length);
    serial_arg[outbuf.length] = 0;
    return true;
}
```

(This deletes `inbuf`, its three assignments, and the commented-out `PassThruIoctl` block that referenced it. Nothing else changes.)

- [ ] **Step 4: `mainwindow.cpp`**

In `MainWindow::calibration_data_treewidget_item_selected`, first confirm that the `else if (ui->calibrationDataTreeWidget->indexOfTopLevelItem(item->parent()) > -1)` block is the last statement in the function (its closing brace is followed directly by the function's closing brace). Then replace this opening:

```cpp
    const QModelIndex index = ui->calibrationDataTreeWidget->selectionModel()->currentIndex();
    QString selectedText = index.data(Qt::DisplayRole).toString();
    int hierarchyLevel = 1;
    QModelIndex seekRoot = index;
    QString selectedRom;

    selectedText = item->text(0);

    while (seekRoot.parent() != QModelIndex())
    {
        seekRoot = seekRoot.parent();
        hierarchyLevel++;
    }

    if (ui->calibrationDataTreeWidget->indexOfTopLevelItem(item) > -1)
    {
        hierarchyLevel = 1;
    }
    else if (ui->calibrationDataTreeWidget->indexOfTopLevelItem(item->parent()) > -1)
    {
        hierarchyLevel = 2;

        QTreeWidgetItem *selectedFilesTreeItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);
```

with:

```cpp
    const QModelIndex index = ui->calibrationDataTreeWidget->selectionModel()->currentIndex();
    QString selectedText = index.data(Qt::DisplayRole).toString();
    QString selectedRom;

    selectedText = item->text(0);

    // A top-level item is a category header; selecting it opens nothing.
    if (ui->calibrationDataTreeWidget->indexOfTopLevelItem(item) > -1)
    {
        return;
    }
    if (ui->calibrationDataTreeWidget->indexOfTopLevelItem(item->parent()) > -1)
    {
        QTreeWidgetItem *selectedFilesTreeItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);
```

The `while` loop only walked `seekRoot` to compute `hierarchyLevel`; `QModelIndex::parent()` has no side effects. Because the `else if` was the last statement, an early `return` is the same control flow.

Further down the same function, delete `int map_index = 0;` and the `map_index++;` inside the `foreach` loop that follows it.

In `MainWindow::add_new_logger_definition_file` and `MainWindow::remove_logger_definition_file`, delete the body line `QObject *obj = sender();`, leaving each body empty.

- [ ] **Step 5: `definition_file_convert.cpp`**

`line_index` is only ever written. Delete all four lines that touch it: `int line_index = 0;`, both `line_index++;`, and `line_index = 0;`. Confirm with `grep -n line_index src/ui/desktop/widgets/definition_file_convert.cpp`, which must print nothing afterwards.

- [ ] **Step 6: `tst_mut_dma_integration.cpp`**

In both tests that declare it, delete:

```cpp
        MockOpenPort& mock = *mockThread.mock;
```

Keep the `MockOpenPortThread mockThread(master);` line above it; its lifetime drives the mock.

- [ ] **Step 7: Verify**

Run: `bazel build --config=release //... 2>&1 | grep -E "Wunused-variable|Wunused-but-set-variable"`
Expected: no output.
Run: `$TMPDIR/gcc15_check.sh src/platform/desktop/unix/j2534/driver/J2534_unix.cpp src/platform/desktop/common/serial/direct/common/serial_port_actions_direct.cpp src/ui/desktop/widgets/mainwindow.cpp tests/tst_mut_dma_integration.cpp | grep -E "Wunused-(but-set-)?variable"`
Expected: no output.
Run: `bazel test --config=release //src/ui/desktop/widgets:all //src/platform/desktop/... //tests:all`
Expected: all pass.

- [ ] **Step 8: Commit**

```bash
git add src/platform/desktop/unix/j2534/driver/J2534_unix.cpp \
        src/platform/desktop/common/serial/direct/common/serial_port_actions_direct.cpp \
        src/ui/desktop/widgets/mainwindow.cpp src/ui/desktop/widgets/definition_file_convert.cpp \
        tests/tst_mut_dma_integration.cpp
git commit -m "fix: delete unused and set-but-unused locals

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

### Task 2: Replace the deprecated `QCheckBox::stateChanged`

**Files:**
- Modify: `src/ui/desktop/widgets/calibration_maps.cpp:285`
- Test: `src/ui/desktop/widgets/calibration_maps_test.cpp` (the test that records `CalibrationMaps::checkbox_state_changed`)

**Interfaces:**
- Consumes: nothing.
- Produces: `CalibrationMaps::checkbox_state_changed(int)` keeps its signature and its values (`Qt::Checked` = 2, `Qt::Unchecked` = 0).

- [ ] **Step 1: Pin the emitted values in a new test**

In `calibration_maps_test.cpp`, add `#include <tuple>` to the standard includes, and add this test directly after `TEST(CalibrationMapsTest, retainedSwitchRefreshKeepsUncheckedControlWithoutEmittingEdits)` (its setup is the same):

```cpp
TEST(CalibrationMapsTest, switchCheckboxEmitsQtCheckStateValues)
{
    MapFixture fixture;
    const auto id = fixture.open(numeric_table("1D", 1, 1));
    ASSERT_TRUE(id.has_value());
    auto *session = fixture.workspace.find(*id);
    auto definition = *session->definition();
    definition.definition.maps[0].type = "Switch";
    replace_definition(*session, std::move(definition));
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *checkbox = qobject_cast<QCheckBox *>(table_of(map)->cellWidget(0, 0));
    ASSERT_TRUE(checkbox != nullptr);
    fastecu::testing::SignalRecorder edits(&map, &CalibrationMaps::checkbox_state_changed);
    checkbox->setChecked(true);
    checkbox->setChecked(false);
    ASSERT_EQ(edits.count(), 2u);
    EXPECT_EQ(std::get<0>(edits.snapshot().at(0)), static_cast<int>(Qt::Checked));
    EXPECT_EQ(std::get<0>(edits.snapshot().at(1)), static_cast<int>(Qt::Unchecked));
}
```

- [ ] **Step 2: Run the test against the current code**

Run: `bazel test --config=release //src/ui/desktop/widgets:test_calibration_maps`
Expected: PASS. This pins today's behavior before the signal changes.

- [ ] **Step 3: Switch to `checkStateChanged`**

In `calibration_maps.cpp`, replace:

```cpp
            connect(checkbox, &QCheckBox::stateChanged, this, &CalibrationMaps::checkbox_state_changed);
```

with:

```cpp
            connect(checkbox, &QCheckBox::checkStateChanged, this,
                    [this](Qt::CheckState state) { emit checkbox_state_changed(static_cast<int>(state)); });
```

- [ ] **Step 4: Verify**

Run: `bazel test --config=release //src/ui/desktop/widgets:test_calibration_maps`
Expected: PASS.
Run: `bazel build --config=release //src/ui/desktop/widgets:all 2>&1 | grep Wdeprecated-declarations`
Expected: no output.

- [ ] **Step 5: Commit**

```bash
git add src/ui/desktop/widgets/calibration_maps.cpp src/ui/desktop/widgets/calibration_maps_test.cpp
git commit -m "fix: connect the map switch checkbox through checkStateChanged

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

### Task 3: Make signed/unsigned comparisons agree

**Files:**
- Modify: `src/algorithms/protocol/ssm/ssm_protocol_core.cpp` (`transformWord`)
- Modify: `src/backend/protocol/testing/scripted_ssm_transport.h` (`wIdx_`)
- Modify: `src/ui/desktop/biu/biu_operations_subaru.cpp` (header-length check, `parse_message_to_hex`)
- Modify (tests, whole-file sweep): `apps/desktop/desktop_composition_test.cpp`, `src/algorithms/checksum/checksum_test.cpp`, `src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol_test.cpp`, `src/algorithms/protocol/mut_dma/freeform_test.cpp`, `src/algorithms/protocol/mut_dma/memory_test.cpp`, `src/algorithms/protocol/mut_dma/mut_dma_test.cpp`, `src/backend/flash/ecu/subaru_denso_sh7055_02_plan_test.cpp`, `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp`, `src/backend/logging/logger_model_test.cpp`, `src/platform/desktop/common/connection/adapter_connection_test.cpp`, `src/platform/desktop/common/diagnostics/workers/dtc_worker_test.cpp`, `src/platform/desktop/common/diagnostics/workers/ssm_identify_worker_test.cpp`, `src/platform/desktop/common/flash/worker/flash_worker_test.cpp`, `src/platform/desktop/common/logging/logging_adapters_test.cpp`, `src/platform/desktop/common/logging/runtime/logging_engine_test.cpp`, `src/platform/desktop/common/logging/runtime/logging_worker_test.cpp`, `src/platform/desktop/common/ports/qt_port_adapters_test.cpp`, `src/platform/desktop/common/service_functions/worker/service_function_worker_test.cpp`, `src/ui/desktop/widgets/calibration_maps_test.cpp`, `src/ui/desktop/widgets/mainwindow_test.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `ScriptedSsmTransport`'s private `wIdx_` becomes `std::size_t` (matching `ScriptedCanFlashTransport` and `ScriptedKlineFlashTransport`); no public signature changes.

- [ ] **Step 1: Record the GCC sign-compare sites**

Run: `$TMPDIR/gcc15_check.sh > /tmp/gcc.log 2>&1; grep -B1 -E "Wsign-compare" /tmp/gcc.log | grep -E "required from here|^(\./)?(src|apps|tests)/.*Wsign-compare"`
Expected: the production sites in Steps 2–4 and one instantiation site in each test file listed above.

- [ ] **Step 2: `ssm_protocol_core.cpp`**

In `transformWord`, `Rounds` is a `std::size_t` template parameter. Replace:

```cpp
    for (int r = 0; r < Rounds; ++r)
    {
        const int ki = reverse ? (Rounds - 1 - r) : r;
```

with:

```cpp
    for (std::size_t r = 0; r < Rounds; ++r)
    {
        const std::size_t ki = reverse ? (Rounds - 1 - r) : r;
```

`r` and `ki` stay within `[0, Rounds)`, so every value is unchanged; `keytogenerateindex[ki]` now indexes the span with its own size type.

- [ ] **Step 3: `scripted_ssm_transport.h`**

Replace `int wIdx_ = 0;` with `std::size_t wIdx_ = 0;`. Its uses (`wIdx_ == expected_.size()`, `wIdx_ >= expected_.size()`, `expected_.at(wIdx_)`, `++wIdx_`) all compare or index with `std::size_t`.

- [ ] **Step 4: `biu_operations_subaru.cpp`**

Replace:

```cpp
    if (((uint8_t)message.at(0) & 0x7FU) != (uint8_t)message.length() - 4)
```

with:

```cpp
    if (((uint8_t)message.at(0) & 0x7F) != (uint8_t)message.length() - 4)
```

Both sides are now `int`. The left side is in `[0, 127]` either way; when the right side is negative, the old unsigned comparison and the new signed one both report "not equal".

In `BiuOperationsSubaru::parse_message_to_hex`, replace `for (unsigned long i = 0; i < received.length(); i++)` with `for (qsizetype i = 0; i < received.length(); i++)`.

- [ ] **Step 5: Fix the reported test lines**

Apply these exact edits:

| File | Before | After |
| --- | --- | --- |
| `checksum_test.cpp` | `EXPECT_EQ(bytes::readU32Be(result.romData, 0x1FF8F0), 0x5AA5A55A);` | `EXPECT_EQ(bytes::readU32Be(result.romData, 0x1FF8F0), 0x5AA5A55Au);` |
| `freeform_test.cpp` | `ASSERT_EQ(reqLen(1), ((1 + 3) >> 2) + 1 * 2 + 0x1c); // 1 + 2 + 28 = 31` | `ASSERT_EQ(reqLen(1), std::size_t{((1 + 3) >> 2) + 1 * 2 + 0x1c}); // 1 + 2 + 28 = 31` |
| `freeform_test.cpp` | `ASSERT_EQ(reqLen(4), ((4 + 3) >> 2) + 4 * 2 + 0x1c); // 1 + 8 + 28 = 37` | `ASSERT_EQ(reqLen(4), std::size_t{((4 + 3) >> 2) + 4 * 2 + 0x1c}); // 1 + 8 + 28 = 37` |
| `freeform_test.cpp`, `mut_dma_test.cpp` | `ASSERT_EQ(static_cast<int>(f.size()), reqLen(2));` | `ASSERT_EQ(f.size(), reqLen(2));` |
| `mut_dma_test.cpp` | `EXPECT_EQ(responseDataLength(ch), 2 + 1 + 4);` | `EXPECT_EQ(responseDataLength(ch), std::size_t{2 + 1 + 4});` |
| `subaru_tcu_denso_sh705x_can_executor_test.cpp` | `EXPECT_EQ(events.phase_progress_calls.back().done, kReadPageSize);` | `EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(kReadPageSize));` |
| every other listed file | `ASSERT_EQ(<x>.size(), 1);` / `ASSERT_EQ(<recorder>.count(), 0);` etc. | the same with a `u` literal: `1u`, `0u` |

`kReadPageSize` is `0x400`, so the `static_cast<int>` does not change its value; `RecordedPhaseProgress::done` is `int`.

- [ ] **Step 6: Sweep each listed test file**

GCC only reported the first comparison per operand-type pair in each file. In every test file listed under **Files**, find every googletest comparison (`EXPECT_`/`ASSERT_` + `EQ`, `NE`, `LT`, `LE`, `GT`, `GE`) whose two operands differ in signedness, and fix it the same way. Decide by the operand's declared type, not its name:

- Unsigned, so a literal compared with it gets a `u` suffix: `std::vector`/`std::string`/`std::span`/`bytes::Bytes` `.size()`, `fastecu::testing::SignalRecorder::count()` (returns `std::size_t`), and functions returning `std::size_t` or `std::uint32_t`.
- Signed, so leave the comparison alone: Qt containers' `size()`/`count()`/`length()` (`qsizetype`), `std::chrono` `count()` (for example `ASSERT_EQ(clock_view->elapsed().count(), 3000);` in `ssm_identify_worker_test.cpp`), and anything returning `int`.

A starting list of candidates in a file:

```bash
grep -n -E "(EXPECT|ASSERT)_(EQ|NE|LT|LE|GT|GE)\(.*\.(size|count|length)\(\)" <file>
```

- [ ] **Step 7: Re-run GCC until silent**

Run: `$TMPDIR/gcc15_check.sh <every file modified in this task> | grep -E "Wsign-compare"`
Expected: no output. If a new instantiation site appears, it is the next comparison of the same type pair in that file; fix it as in Step 6 and re-run.

- [ ] **Step 8: Verify behavior**

Run: `bazel test --config=release //src/algorithms/... //src/backend/... //src/platform/desktop/... //src/ui/desktop/... //apps/... //tests:all`
Expected: all pass. `//src/algorithms/protocol/ssm:ssm_protocol_core_test` holds the known-answer seed/key vectors that pin Step 2.

- [ ] **Step 9: Commit**

```bash
git add -u
git commit -m "fix: make signed and unsigned comparisons agree

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

### Task 4: Check `write()` results in POSIX test fixtures

**Files:**
- Modify: `src/platform/desktop/common/serial/direct/unix/direct_backend_pty_test.cpp` (four `::write` calls)
- Modify: `src/platform/desktop/unix/j2534/testing/mock_openport.h` (one `::write` call)
- Modify: `tests/serial_pty_e2e_test.cpp` (one `::write` call)
- Modify: `tests/tst_mut_dma_integration.cpp` (`injectDataFrame`, `reply`)

**Interfaces:**
- Consumes: nothing.
- Produces: no interface changes.

glibc declares `write()` with `warn_unused_result` under `_FORTIFY_SOURCE`, and GCC ignores a `(void)` cast on such a call. Where googletest is available, assert the byte count; `mock_openport.h` has no googletest dependency, so it discards explicitly.

- [ ] **Step 1: Record the sites**

Run: `$TMPDIR/gcc15_check.sh src/platform/desktop/common/serial/direct/unix/direct_backend_pty_test.cpp tests/serial_pty_e2e_test.cpp tests/tst_mut_dma_integration.cpp tests/tst_serial_port_crash.cpp | grep Wunused-result`
Expected: four sites in `direct_backend_pty_test.cpp`, one in `serial_pty_e2e_test.cpp`, two in `tst_mut_dma_integration.cpp`, and `mock_openport.h:79` (through `tst_serial_port_crash.cpp`).

- [ ] **Step 2: `direct_backend_pty_test.cpp`**

Inside the `responder` thread lambda, replace:

```cpp
            ::write(master, "\x80\xf0\x10\x02", 4);
            QThread::msleep(30);
            ::write(master, "\xaa\xbb\xcc", 3);
```

with:

```cpp
            EXPECT_EQ(::write(master, "\x80\xf0\x10\x02", 4), 4);
            QThread::msleep(30);
            EXPECT_EQ(::write(master, "\xaa\xbb\xcc", 3), 3);
```

Replace `::write(master, "\x11\x22\x33", 3); // junk arrives...` with `ASSERT_EQ(::write(master, "\x11\x22\x33", 3), 3); // junk arrives...`.

Replace `::write(master, "\x82\x10\xf1\xaa\xbb\xcc\xdd", 7);` with `ASSERT_EQ(::write(master, "\x82\x10\xf1\xaa\xbb\xcc\xdd", 7), 7);`.

(`EXPECT_EQ` in the thread, because `ASSERT_` would only return from the lambda.)

- [ ] **Step 3: `serial_pty_e2e_test.cpp`**

Replace `::write(master, reply.data(), 7);` with `EXPECT_EQ(::write(master, reply.data(), 7), 7);`.

- [ ] **Step 4: `tst_mut_dma_integration.cpp`**

In `injectDataFrame`, replace `::write(fd, f.constData(), f.size());` with `EXPECT_EQ(::write(fd, f.constData(), f.size()), f.size());` (`ssize_t` and `qsizetype` are both signed).

In `reply`, replace `::write(fd, s, qstrlen(s));` with:

```cpp
        const auto length = static_cast<ssize_t>(qstrlen(s));
        EXPECT_EQ(::write(fd, s, qstrlen(s)), length);
```

- [ ] **Step 5: `mock_openport.h`**

Add `#include <tuple>` to the includes, then replace `::write(fd, resp.constData(), resp.size());` with:

```cpp
            // A short write surfaces as a missing reply, which the caller's
            // read timeout reports.
            std::ignore = ::write(fd, resp.constData(), resp.size());
```

- [ ] **Step 6: Verify**

Run: `$TMPDIR/gcc15_check.sh src/platform/desktop/common/serial/direct/unix/direct_backend_pty_test.cpp tests/serial_pty_e2e_test.cpp tests/tst_mut_dma_integration.cpp tests/tst_serial_port_crash.cpp | grep Wunused-result`
Expected: no output.
Run: `bazel test --config=release //src/platform/desktop/common/serial/direct/unix:all //src/platform/desktop/unix/j2534/... //tests:all`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/platform/desktop/common/serial/direct/unix/direct_backend_pty_test.cpp \
        src/platform/desktop/unix/j2534/testing/mock_openport.h \
        tests/serial_pty_e2e_test.cpp tests/tst_mut_dma_integration.cpp
git commit -m "fix: check write() results in POSIX test fixtures

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

### Task 5: Bind range-for structured bindings by reference

**Files:**
- Modify: `src/backend/checksum/dispatch_test.cpp:371`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp:862`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp:838`

**Interfaces:** none.

- [ ] **Step 1: Edit**

| File | Before | After |
| --- | --- | --- |
| `dispatch_test.cpp` | `for (const auto [flash_method, mcu_type, size] :` | `for (const auto& [flash_method, mcu_type, size] :` |
| `subaru_denso_sh7058_can_diesel_executor_test.cpp` | `for (const auto [reply, expected] : {std::pair{...` | `for (const auto& [reply, expected] : {std::pair{...` |
| `subaru_denso_sh7058_can_executor_test.cpp` | `for (const auto [b6_reply, expected] : {std::pair{...` | `for (const auto& [b6_reply, expected] : {std::pair{...` |

Only `const auto` becomes `const auto&`; the rest of each line is unchanged.

- [ ] **Step 2: Verify**

Run: `$TMPDIR/gcc15_check.sh src/backend/checksum/dispatch_test.cpp src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp | grep Wrange-loop-construct`
Expected: no output.
Run: `bazel test --config=release //src/backend/checksum:all //src/backend/flash/ecu:all`
Expected: all pass.

- [ ] **Step 3: Commit**

```bash
git add src/backend/checksum/dispatch_test.cpp \
        src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp \
        src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp
git commit -m "fix: bind range-for structured bindings by reference

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

### Task 6: Rewrite byte-vector growth that GCC 15 misdiagnoses

**Files:**
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp:708`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp:595`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp:513`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp:517`
- Modify: `src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor_test.cpp:104,254`
- Modify: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan_test.cpp:46-47`
- Test (existing, unchanged): `subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp` `ReadReturnsTheWindowPaddedWithFF`, `subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp` `ReadReturnsTheLowerWindowPaddedWithFF`

**Interfaces:** none.

GCC 15.3 at `-O2` reports `-Warray-bounds` inside libstdc++ for these `insert`/`resize` calls; the code is correct. Every rewrite below was compiled clean with GCC 15.3.

- [ ] **Step 1: Confirm the padded-read tests pass before the change**

Run: `bazel test --config=release //src/backend/flash/ecu:all`
Expected: PASS, including the two `ReadReturns…PaddedWithFF` tests.

- [ ] **Step 2: Executors**

In both executors replace:

```cpp
        rom.insert(rom.end(), window->begin(), window->end());
```

with:

```cpp
        rom.append_range(*window);
```

- [ ] **Step 3: Tests**

| File | Before | After |
| --- | --- | --- |
| `subaru_denso_sh7058_can_diesel_executor_test.cpp` | `payload.insert(payload.end(), chunk.begin(), chunk.end());` | `payload.append_range(chunk);` |
| `subaru_denso_sh7058_can_executor_test.cpp` | `write_payload.insert(write_payload.end(), chunk.begin(), chunk.end());` | `write_payload.append_range(chunk);` |
| `subaru_hitachi_m32r_kline_executor_test.cpp` (both occurrences) | `request.insert(request.end(), encrypted.begin() + address, encrypted.begin() + address + 0x80);` | `request.append_range(bytes::ByteView(encrypted).subspan(address, 0x80));` |

In `subaru_unisia_jecs_m32r_bootmode_plan_test.cpp` replace:

```cpp
        bytes::Bytes expected(200, 0x5a);
        expected.resize(256, 0x00); // upload_kernel() :312-315
```

with:

```cpp
        bytes::Bytes expected(256, 0x00); // upload_kernel() :312-315 pads to 256
        std::fill_n(expected.begin(), 200, bytes::Byte{0x5a});
```

and add `#include <algorithm>` to that file if it is not already included.

- [ ] **Step 4: Verify with GCC**

Run: `$TMPDIR/gcc15_check.sh src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor_test.cpp src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan_test.cpp | grep Warray-bounds`
Expected: no output.

- [ ] **Step 5: Verify behavior**

Run: `bazel test --config=release //src/backend/flash/ecu:all`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add src/backend/flash/ecu/
git commit -m "fix: grow byte vectors with append_range to avoid GCC 15 false positives

GCC 15.3 at -O2 reports -Warray-bounds inside libstdc++ for insert-at-end
and resize on a byte vector that already holds data. The code was correct;
append_range (and building the boot-mode expectation at full size) says
the same thing without tripping the diagnostic.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

- [ ] **Step 7: Android check (deferred to CI)**

`append_range` needs the Android NDK's libc++ to support it. If the portable-core CI job later fails to compile the two executors, replace `rom.append_range(*window);` with the size-then-copy form and re-verify with GCC:

```cpp
        bytes::Bytes rom(kReadRegion.start + window->size(), 0xFF);
        std::ranges::copy(*window, rom.begin() + static_cast<std::ptrdiff_t>(kReadRegion.start));
```

(Remove the now-redundant `bytes::Bytes rom(kReadRegion.start, 0xFF);` line above it.)

### Task 7: One idiom for intentionally discarded results

**Files:**
- Modify: `docs/coding-style.md` (Error handling section)
- Modify: `src/backend/config/provisioning.cpp:126`
- Modify: `src/backend/config/provisioning_test.cpp:116,132`
- Modify: `src/backend/ports/testing/in_memory_file_system_test.cpp:53`
- Modify (21 `static_cast<void>(call)` sites): `src/backend/diagnostics/dtc_session.cpp:62,63,118,129,136`, `src/backend/diagnostics/dtc_session_test.cpp:186,199,212,288,303`, `src/backend/ports/testing/fake_cancellation_token_test.cpp:68`, `src/backend/protocol/testing/fake_diagnostic_link_test.cpp:86,87`, `src/ui/desktop/biu/biu_operations_subaru.cpp:354`, `src/ui/desktop/diagnostic_link_io.h:30`, `src/ui/desktop/widgets/dataterminal.cpp:253,350`, `src/ui/desktop/widgets/dtc_operations.cpp:124`, `src/ui/desktop/widgets/mainwindow.cpp:217,221`, `src/ui/desktop/widgets/menu_actions.cpp:758`
- Modify (17 `(void)call` sites): `src/backend/calibration/calibration_service.cpp:172`, `src/backend/config/app_config.cpp:159`, `src/backend/ports/testing/mock_clock_test.cpp:114,115`, `src/backend/protocol/uds/testing/scripted_uds_channel_test.cpp:72,73,98,100`, `src/backend/protocol/uds/uds_client_test.cpp:63,237`, `src/platform/desktop/common/flash/flash_workflow_test.cpp:357`, `src/platform/desktop/common/testing/event_helpers.h:41`, `src/ui/desktop/calibration/hex_parse_qt_compat_test.cpp:62,75`, `src/ui/desktop/calibration/qt_calibration_interaction_test.cpp:350`, `src/ui/desktop/widgets/mainwindow.cpp:1436`, `src/ui/desktop/widgets/settings.cpp:46`

**Interfaces:** none.

- [ ] **Step 1: Write the rule**

In `docs/coding-style.md`, at the end of the **Error handling** section (after the paragraph that ends with the `ErrorKind` ADR link), add:

````markdown
A result discarded on purpose is assigned to `std::ignore` (from `<tuple>`),
with a comment saying why when the reason is not obvious from the call:

```cpp
// A missing previous config is not an error for this step.
std::ignore = fs.copy_file(previous_config_file, target, false);
```

Not `(void)call()` or `static_cast<void>(call())`: `std::ignore =` names the
intent, and GCC ignores a cast on a `warn_unused_result` function. An unused
variable or parameter is still marked `(void)name;`.
````

- [ ] **Step 2: MSVC C4834 sites**

In `provisioning.cpp`, replace:

```cpp
            fs.copy_file(previous_config_file, paths.config_files_directory + "fastecu.cfg", false);
```

with:

```cpp
            std::ignore = fs.copy_file(previous_config_file, paths.config_files_directory + "fastecu.cfg", false);
```

keeping the two comment lines above it, and add `#include <tuple>`.

In `provisioning_test.cpp`, replace `fs.create_directory(paths.config_files_directory);` with `ASSERT_THAT(fs.create_directory(paths.config_files_directory), fastecu::testing::IsOk());`, and `fs.create_directory(paths.syslog_files_directory);` with `ASSERT_THAT(fs.create_directory(paths.syslog_files_directory), fastecu::testing::IsOk());`.

In `in_memory_file_system_test.cpp` (test `FileSystem.RemoveThenNotExists`), replace `fs.create_directory("/a");` with `ASSERT_THAT(fs.create_directory("/a"), fastecu::testing::IsOk());`.

- [ ] **Step 3: Convert the existing discards**

At each of the 38 sites listed under **Files**, rewrite `static_cast<void>(EXPR);` or `(void)EXPR;` as `std::ignore = EXPR;`, keeping any trailing comment, and add `#include <tuple>` to the file's includes once. For example:

```cpp
        static_cast<void>(link_.set_p1_max(35ms)); // result never checked today
```

becomes:

```cpp
        std::ignore = link_.set_p1_max(35ms); // result never checked today
```

Leave `static_cast<void>(level);` in the three SH705x/SH7058 executor tests and every `(void)name;` alone: those mark unused variables, not discarded results. If an `EXPR` turns out to return `void` (the build fails with "no viable overloaded '='"), delete the cast instead; there is nothing to discard.

Confirm none remain:

```bash
git grep -n -E "static_cast<void>\([^)]*\(" -- '*.cpp' '*.h'
git grep -n -E "\(void\) ?[A-Za-z_:.>-]+\(" -- '*.cpp' '*.h'
```

Expected: no output.

- [ ] **Step 4: Verify**

Run: `bazel build --config=release //... 2>&1 | grep -E "warning:|error:"`
Expected: no output.
Run: `bazel test --config=release //...`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add -u docs/coding-style.md
git commit -m "refactor: discard results through std::ignore

Make std::ignore the one idiom for an intentionally discarded result and
convert the existing void casts. MSVC's std::expected is [[nodiscard]], so
provisioning's deliberate copy_file discard warned (C4834) there; the
three test call sites now assert the result instead of discarding it.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

### Task 8: Remove stale flags and the dead suppression

**Files:**
- Modify: `tests/force_asserts/BUILD.bazel`
- Modify: `tests/BUILD.bazel` (`j2534_bridge_protocol_test`, `j2534_bridge_client_test`, `j2534_win_bridge_test`)
- Modify: `bazel/qt_common.bzl` (`COMMON_COPTS`)
- Modify: `bazel/qt_remote_objects.bzl`

**Interfaces:**
- Produces: `COMMON_COPTS` becomes the plain list `["-DQT_FORCE_ASSERTS", "-DQT_DEPRECATED_WARNINGS"]`.

- [ ] **Step 1: `tests/force_asserts/BUILD.bazel`**

`--macos_minimum_os=26.0` in `.bazelrc` already sets the deployment target; the per-target `-mmacosx-version-min=10.15` makes libc++ emit "The selected platform is no longer supported", and `-Wno-implicit-function-declaration` only affects C. Replace:

```starlark
    copts = [
        "-DQT_FORCE_ASSERTS",
        "-DQT_NO_DEBUG",
    ] + select({
        "@platforms//os:macos": [
            "-mmacosx-version-min=10.15",
            "-Wno-implicit-function-declaration",
        ],
        "//conditions:default": [],
    }),
```

with:

```starlark
    copts = [
        "-DQT_FORCE_ASSERTS",
        "-DQT_NO_DEBUG",
    ],
```

- [ ] **Step 2: `tests/BUILD.bazel`**

None of the three bridge tests calls `assert()` any more, so `/UNDEBUG` only produces MSVC's D9025 override warning. Replace the comment

```starlark
# Windows wire-format probe; assert() is its test protocol, so release builds
# explicitly re-enable assertions and retain the handwritten main.
```

with `# Windows wire-format probe.`, delete `copts = ["/UNDEBUG"],` from `j2534_bridge_protocol_test` and `j2534_bridge_client_test`, and delete `copts = COMMON_COPTS + ["/UNDEBUG"],` from `j2534_win_bridge_test` (`fastecu_gtest` already adds `COMMON_COPTS` when `qt = True`). If `COMMON_COPTS` is then unused in the file, remove it from the `load` statement.

- [ ] **Step 3: `bazel/qt_common.bzl`**

Replace:

```starlark
COMMON_COPTS = [
    "-DQT_FORCE_ASSERTS",
    "-DQT_DEPRECATED_WARNINGS",
] + select({
    "@platforms//os:macos": [
        "-Wno-implicit-function-declaration",
    ],
    "//conditions:default": [],
})
```

with:

```starlark
COMMON_COPTS = [
    "-DQT_FORCE_ASSERTS",
    "-DQT_DEPRECATED_WARNINGS",
]
```

- [ ] **Step 4: `bazel/qt_remote_objects.bzl`**

Delete the `copts = select({...}),` argument (the one holding only `-Wno-implicit-function-declaration`) from the `cc_library` call.

- [ ] **Step 5: Verify**

Run: `python3 scripts/gazelle_check.py --fix && git status --short`
Expected: no BUILD changes beyond this task's edits.
Run: `bazel build --config=release //... 2>&1 | grep -E "warning:|error:"`
Expected: no output (the force-asserts test no longer prints the libc++ `#warning`).
Run: `bazel test --config=release //tests/force_asserts:tst_force_asserts_test //src/platform/desktop/common/remote_utility/... //src/platform/desktop/common/serial/remote/...`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add tests/force_asserts/BUILD.bazel tests/BUILD.bazel bazel/qt_common.bzl bazel/qt_remote_objects.bzl
git commit -m "build: drop stale and dead warning-related flags

-mmacosx-version-min=10.15 on the force-asserts test made libc++ emit a
#warning; the global --macos_minimum_os already applies. /UNDEBUG on the
J2534 bridge tests only produced MSVC D9025 since they stopped using
assert(). -Wno-implicit-function-declaration names a C-only diagnostic.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

### Task 9: Clear the linker warnings

**Files:**
- Create: `bazel/patches/rules_qt_mac_single_rpath.patch`
- Modify: `bazel/patches/BUILD.bazel` (`exports_files`)
- Modify: `MODULE.bazel` (`single_version_override` for `rules_qt`)
- Modify: `tests/fake_j2534_dll.def`

**Interfaces:**
- Produces: in `@qt_mac_aarch64`, a new `:qt_rpath_mac` library that every `qt_<module>_mac` library depends on.

- [ ] **Step 1: Measure the duplicate rpath**

Run: `bazel aquery --config=release 'mnemonic("CppLink", //apps/desktop:fastecu)' 2>/dev/null | grep -o -- "-rpath [^ ]*qt_mac_aarch64/lib" | wc -l`
Expected: more than 1 (one per linked Qt module).

- [ ] **Step 2: Write the patch**

The template is `extension/qt/6.8.3/mac_aarch64.BUILD` in `rules_qt` 0.0.6. Generate the patch from a copy:

```bash
ob=$(bazel info output_base)
src=$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$ob/external/rules_qt+")
work=$(mktemp -d)
mkdir -p "$work/a/extension/qt/6.8.3" "$work/b/extension/qt/6.8.3"
cp "$src/extension/qt/6.8.3/mac_aarch64.BUILD" "$work/a/extension/qt/6.8.3/"
cp "$src/extension/qt/6.8.3/mac_aarch64.BUILD" "$work/b/extension/qt/6.8.3/"
```

Edit `$work/b/extension/qt/6.8.3/mac_aarch64.BUILD`. Insert this block immediately before the `[` that opens the `qt_%s_mac` list comprehension:

```starlark
# Every module links against the same framework directory. Carrying its rpath
# on one shared library emits it once per link: Apple's ld warns on each
# repeated -rpath, and FastECU treats link warnings as errors.
cc_library(
    name = "qt_rpath_mac",
    additional_linker_inputs = [":lib"],
    linkopts = ["-rpath $(rootpath :lib)"],
    target_compatible_with = ["@platforms//os:osx"],
)

```

Inside the comprehension's `cc_library`, replace:

```starlark
        linkopts = ["-F $(location :lib)"] + [
            "-framework %s" % _framework_names[library_name],  # macOS qt libs do not contain a 6 - e.g. instead of Qt6Core the lib is called QtCore
            "-rpath $(rootpath :lib)",
        ],
```

with:

```starlark
        linkopts = ["-F $(location :lib)"] + [
            "-framework %s" % _framework_names[library_name],  # macOS qt libs do not contain a 6 - e.g. instead of Qt6Core the lib is called QtCore
        ],
        deps = [":qt_rpath_mac"],
```

Then write the patch:

```bash
(cd "$work" && diff -u a/extension/qt/6.8.3/mac_aarch64.BUILD b/extension/qt/6.8.3/mac_aarch64.BUILD) \
    > bazel/patches/rules_qt_mac_single_rpath.patch || true
head -3 bazel/patches/rules_qt_mac_single_rpath.patch
```

Expected: the header names `a/extension/qt/6.8.3/mac_aarch64.BUILD` and `b/extension/qt/6.8.3/mac_aarch64.BUILD` (matching `patch_strip = 1`).

- [ ] **Step 3: Register the patch**

In `bazel/patches/BUILD.bazel`, add `"rules_qt_mac_single_rpath.patch",` to `exports_files`, keeping the list sorted. In `MODULE.bazel`, add `"//bazel/patches:rules_qt_mac_single_rpath.patch",` to the `rules_qt` `single_version_override` `patches` list, after `rules_qt_drop_intel_mac.patch`, and extend the comment above it with one line: `# mac_single_rpath gives every Qt module's framework rpath to one shared library, so Apple's ld sees it once per link instead of warning on each repeat.`

- [ ] **Step 4: Verify the rpath appears once**

Run: `bazel aquery --config=release 'mnemonic("CppLink", //apps/desktop:fastecu)' 2>/dev/null | grep -o -- "-rpath [^ ]*qt_mac_aarch64/lib" | wc -l`
Expected: `1`.
Run: `bazel build --config=release //... 2>&1 | grep "duplicate -rpath"`
Expected: no output.

- [ ] **Step 5: Verify Qt still loads at runtime**

Run: `bazel test --config=release //src/ui/desktop/widgets:all //src/platform/desktop/...`
Expected: all pass (these tests load the Qt frameworks through that rpath).
Run: `scripts/package-macos.sh /tmp/fastecu-check.zip local-check`
Expected: exits 0 (it checks that `macdeployqt` bundled `QtCore.framework`).

- [ ] **Step 6: LNK4070**

In `tests/fake_j2534_dll.def`, delete the first line, `LIBRARY fake_j2534_dll`, so the linker takes the output name Bazel gives the DLL. Keep `EXPORTS` and the export list. (Verified on Windows CI in Task 14.)

- [ ] **Step 7: Commit**

```bash
git add bazel/patches/rules_qt_mac_single_rpath.patch bazel/patches/BUILD.bazel MODULE.bazel MODULE.bazel.lock tests/fake_j2534_dll.def
git commit -m "build: emit the Qt framework rpath once per macOS link

Each rules_qt module library carried the same -rpath linkopt, so Apple's ld
warned once per Qt module on every link (about 520 warnings per build).
Also drop the fake J2534 DLL's LIBRARY line, which made MSVC's linker warn
(LNK4070) that it differs from Bazel's output name.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

(Omit `MODULE.bazel.lock` from `git add` if `git status` shows it unchanged.)

### Task 10: Verify PR 1 and open it

**Files:** none modified.

- [ ] **Step 1: Clean macOS build shows nothing**

Run the clean macOS build from Shared tooling.
Expected: the `grep` prints nothing — no `warning:` lines, no `duplicate -rpath`.

- [ ] **Step 2: GCC sweep shows nothing**

Run: `bazel build --config=release //... && $TMPDIR/gcc15_check.sh > /tmp/gcc.log 2>&1; grep -E "warning:" /tmp/gcc.log`
Expected: no output. Any `fatal error` lines are files the harness cannot compile; CI covers them in Task 14.

- [ ] **Step 3: Full tests, lint, gazelle**

Run: `bazel test --config=release //...`
Expected: all pass.
Run: `prek run --all-files && python3 scripts/gazelle_check.py`
Expected: both pass.
Run: `bazel run //:clang_tidy_report_changed`
Expected: `0 findings`.

- [ ] **Step 4: Open PR 1 (after the user authorizes pushing)**

```bash
git push -u origin fix/clear-warnings
gh pr create --base master --head fix/clear-warnings \
    --title "fix: clear compiler and linker warnings (#48, 1/2)" \
    --body-file - <<'EOF'
First of two PRs for #48. Clears every compiler and linker warning at today's warning levels; the second PR makes warnings fatal.

- Unused locals deleted (J2534 and serial: deletions only, so no bench re-qualification is needed).
- Deprecated `QCheckBox::stateChanged` replaced; a new test pins the emitted check-state values.
- Signed/unsigned comparisons made to agree; GCC-only test sites swept file by file.
- `write()` results checked in POSIX test fixtures.
- GCC 15 `-Warray-bounds` false positives avoided with `append_range`; the padded-read tests are unchanged and pass.
- `std::ignore` is the one idiom for discarded results (coding style guide updated).
- Stale `-mmacosx-version-min`, `/UNDEBUG`, and `-Wno-implicit-function-declaration` removed.
- Qt framework rpath emitted once per macOS link (rules_qt patch); `LIBRARY` dropped from the fake J2534 DLL's `.def`.

Design: docs/superpowers/specs/2026-10-03-zero-warnings-design.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J
EOF
```

---

## PR 2: `build/warnings-as-errors`

Create the branch from the tip of PR 1: `git switch -c build/warnings-as-errors`.

### Task 11: Turn the feature on

**Files:**
- Create: `REPO.bazel`
- Modify: `bazel/toolchains/windows_x86_msvc/BUILD.bazel`

**Interfaces:**
- Produces: the toolchain feature name `treat_warnings_as_errors`, requested for every main-repository package; on the x86 toolchain a `:treat_warnings_as_errors_feature` target.

- [ ] **Step 1: Create `REPO.bazel`**

```starlark
# Compiler and linker warnings in first-party code are errors; see
# docs/adr/0018-treat-first-party-warnings-as-errors.md. This applies to every
# package in this repository and to none of its external dependencies.
# --features=-treat_warnings_as_errors on the command line turns it off for a
# local build only; never commit an opt-out.
repo(features = ["treat_warnings_as_errors"])
```

- [ ] **Step 2: Give the x86 MSVC toolchain the feature**

In `bazel/toolchains/windows_x86_msvc/BUILD.bazel`, after the `show_includes` `cc_args`, add:

```starlark
cc_args(
    name = "warnings_as_errors",
    actions = [
        "@rules_cc//cc/toolchains/actions:compile_actions",
        "@rules_cc//cc/toolchains/actions:link_actions",
    ],
    args = ["/WX"],
)
```

After the `parse_showincludes` `cc_feature`, add:

```starlark
# Requested for the main repository by REPO.bazel. Undefined features are
# silently ignored by a rule-based toolchain, so without this the J2534
# bridge binaries built here would skip the warnings gate.
cc_feature(
    name = "treat_warnings_as_errors_feature",
    args = [":warnings_as_errors"],
    feature_name = "treat_warnings_as_errors",
)
```

In the `cc_toolchain`, change `known_features = _ENABLED_FEATURES,` to `known_features = _ENABLED_FEATURES + [":treat_warnings_as_errors_feature"],`. Leave `enabled_features` unchanged: the feature is known but only on when requested.

- [ ] **Step 3: Verify the scope with aquery**

Run: `bazel aquery --config=release 'mnemonic("CppCompile", //src/algorithms/checksum:checksum)' 2>/dev/null | grep -c -- "-Werror"`
Expected: `1` or more.
Run: `bazel aquery --config=release 'mnemonic("CppCompile", @googletest//:gtest)' 2>/dev/null | grep -c -- "-Werror"`
Expected: `0`.
Run: `bazel aquery --config=release 'mnemonic("CppLink", //apps/desktop:fastecu)' 2>/dev/null | grep -c -- "fatal_warnings"`
Expected: `1` or more.

- [ ] **Step 4: Verify the build and the escape hatch**

Run: `bazel build --config=release //... && bazel test --config=release //...`
Expected: both succeed.
Then, temporarily, add `int warnings_probe_unused = 0;` inside any function body in `src/algorithms/checksum/checksum_ecu_mitsu_m32r_can.cpp` and run:
`bazel build --config=release //src/algorithms/checksum:checksum` — expected: FAILS with `-Werror,-Wunused-variable`.
`bazel build --config=release --features=-treat_warnings_as_errors //src/algorithms/checksum:checksum` — expected: succeeds with a warning.
Revert the probe: `git checkout src/algorithms/checksum/checksum_ecu_mitsu_m32r_can.cpp`.

- [ ] **Step 5: Commit**

```bash
python3 scripts/gazelle_check.py
git add REPO.bazel bazel/toolchains/windows_x86_msvc/BUILD.bazel
git commit -m "build: treat first-party warnings as errors

REPO.bazel requests the C++ toolchain's treat_warnings_as_errors feature
for every package in this repository, making compile and link warnings
fatal locally and in CI. External repositories are unaffected. The x86
MSVC toolchain defines the feature itself, since a rule-based toolchain
ignores features it does not know.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

### Task 12: Keep clang-tidy from double-gating compiler warnings

**Files:**
- Modify: `scripts/clang_tidy_runner.py` (`run_workflow`, where the `run-clang-tidy` command is assembled)
- Test: `scripts/clang_tidy_runner_test.py`

**Interfaces:**
- Produces: every `run-clang-tidy` invocation carries `-extra-arg=-Wno-error`.

- [ ] **Step 1: Write the failing tests**

In `scripts/clang_tidy_runner_test.py`, add to the same test class as `test_windows_report_builds_parallel_runner_command`:

```python
    def test_report_does_not_turn_compiler_warnings_into_errors(self) -> None:
        # The build is the warnings gate (REPO.bazel); clang-tidy runs a newer
        # LLVM than the build compiler and must not fail on a diagnostic only
        # that LLVM knows.
        for platform_name, compdb_tool, tools in (
            (
                "linux",
                _UNIX_COMPDB_TOOL,
                runner.Tools(clang_tidy="/llvm/bin/clang-tidy", run_clang_tidy="/llvm/bin/run-clang-tidy"),
            ),
            (
                "win32",
                _WINDOWS_COMPDB_TOOL,
                runner.Tools(
                    clang_tidy="C:/LLVM/bin/clang-tidy.exe",
                    run_clang_tidy="C:/LLVM/bin/run-clang-tidy.py",
                ),
            ),
        ):
            with self.subTest(platform=platform_name):
                source = self.root / f"{platform_name}.cpp"
                source.write_text("int source;\n")
                self.write_database([source])
                commands: list[list[str]] = []

                def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
                    commands.append(command)
                    return subprocess.CompletedProcess(command, 0)

                with mock.patch.object(runner, "discover_tools", return_value=tools):
                    runner.run_workflow(
                        mode="report",
                        workspace=self.root,
                        compdb_tool=compdb_tool,
                        platform_name=platform_name,
                        environ={},
                        command_runner=fake_run,
                    )

                analysis_command = next(
                    command for command in commands if "-clang-tidy-binary" in command
                )
                self.assertIn("-extra-arg=-Wno-error", analysis_command)
```

- [ ] **Step 2: Run it to see it fail**

Run: `bazel test //:clang_tidy_runner_test --test_output=errors`
Expected: FAIL — `'-extra-arg=-Wno-error' not found`.

- [ ] **Step 3: Add the argument**

In `scripts/clang_tidy_runner.py`, in `run_workflow`, extend the command list built for `run-clang-tidy` so it reads:

```python
        command = _executable_command(tools.run_clang_tidy, platform_name=platform_name) + [
            "-clang-tidy-binary",
            tools.clang_tidy,
            "-config-file",
            str(workspace / ".clang-tidy"),
            "-p",
            directory,
            # REPO.bazel makes the build fail on warnings, so the extracted
            # commands carry -Werror (/WX on Windows). clang-tidy reports
            # compiler errors whatever its check filter says, and its LLVM is
            # newer than the build compiler; without this a diagnostic only
            # that LLVM knows would fail this gate. The build is the warnings
            # gate.
            "-extra-arg=-Wno-error",
        ]
```

- [ ] **Step 4: Run the tests**

Run: `bazel test //:clang_tidy_runner_test --test_output=errors`
Expected: PASS (all tests).

- [ ] **Step 5: Run clang-tidy for real**

Run: `bazel run //:clang_tidy_report_changed`
Expected: completes; `0 findings`. (Windows is checked by CI in Task 14. If Windows clang-tidy rejects `-Wno-error` as an unknown argument, use `"/WX-"` when `platform_name == "win32"` and `"-Wno-error"` otherwise, and make the test expect the platform's spelling.)

- [ ] **Step 6: Commit**

```bash
git add scripts/clang_tidy_runner.py scripts/clang_tidy_runner_test.py
git commit -m "build: keep clang-tidy from failing on compiler warnings

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

### Task 13: Record the decision

**Files:**
- Create: `docs/adr/0018-treat-first-party-warnings-as-errors.md`
- Modify: `docs/adr/README.md` (index)
- Modify: `CLAUDE.md` (Build-graph guardrails)

- [ ] **Step 1: Write the ADR**

Create `docs/adr/0018-treat-first-party-warnings-as-errors.md`:

````markdown
# ADR 0018: First-Party Warnings Are Errors

## Status

Accepted and implemented by `REPO.bazel`.

## Context

Compiler and linker warnings accumulated unnoticed
([issue #48](https://github.com/RcusStackwalker/FastECU/issues/48)): about
fifty distinct sites across Apple clang, GCC, and MSVC, plus 520 duplicate
`-rpath` linker warnings on every macOS build. A warning only prints when its
action runs, and Bazel's local and remote caches replay outputs without their
diagnostics, so an incremental or cached build hides every warning it already
produced. Counting warnings is unreliable; failing on them is not.

## Decision

`REPO.bazel` requests the C++ toolchain's own `treat_warnings_as_errors`
feature for every package in the main repository:

```starlark
repo(features = ["treat_warnings_as_errors"])
```

- Compile and link warnings both fail: `-Werror` and `-Wl,-fatal_warnings`
  (macOS) or `-Wl,-fatal-warnings` (Linux) on clang and GCC, `/WX` for the
  MSVC compiler and linker.
- Only the main repository is affected. External repositories (googletest,
  pugixml, Qt) compile as they always have; warnings their headers raise in
  our code are ours.
- It is always on, locally and in CI, with no `--config`, so local builds and
  CI fail on the same things and share cache keys.
- No target opts out. A negative feature wins, so
  `--features=-treat_warnings_as_errors` on the command line turns it off for
  one local build, for example after a toolchain upgrade adds a diagnostic.
  It never goes into committed configuration.
- The auto-configured clang, GCC, and MSVC toolchains define the feature.
  The hand-written x86 MSVC toolchain in
  `bazel/toolchains/windows_x86_msvc` defines its own, because a rule-based
  toolchain silently ignores a feature it does not know. `rules_android_ndk`
  0.1.5 does not define it, so the Android cross-compile is not gated; the
  desktop builds compile the same portable code.
- clang-tidy runs with `-Wno-error`: it analyzes compile commands that carry
  `-Werror`, reports compiler errors regardless of its check filter, and uses
  a newer LLVM than the build. The build is the warnings gate.
- Warning levels are unchanged: `-Wall` on clang and GCC, MSVC's default.
  Raising them is [issue #470](https://github.com/RcusStackwalker/FastECU/issues/470).

## Alternatives

- `--copt=-Werror` in `.bazelrc`: reaches external repositories, which this
  project does not control, and needs a different spelling per compiler.
- A CI-only `--config=werror`: local builds stop failing on what CI fails
  on, and the two get different action keys, so they stop sharing cache
  entries.
- Per-target `copts` through wrapper macros: every plain `cc_library` would
  move to a macro through Gazelle, and any target written without it would
  silently escape.

## Consequences

- A compiler upgrade on a CI image or a developer machine can fail the build
  on a new diagnostic. The fix goes forward; the command-line negative
  feature only unblocks local work meanwhile.
- A warning is a failed action and is never cached, so a cached build can no
  longer hide one.
- Warnings are fixed at their cause. Source-level `#pragma` suppressions,
  `-Wno-*` flags, and `-Wno-error=` are not used; an intentionally discarded
  result is written `std::ignore = call;` (see the
  [coding style guide](../coding-style.md#error-handling)).
````

- [ ] **Step 2: Index it**

In `docs/adr/README.md`, add this row after the 0017 row:

```markdown
| [0018](0018-treat-first-party-warnings-as-errors.md) | Compiler and linker warnings in first-party code are errors |
```

- [ ] **Step 3: Add the CLAUDE.md rule**

In `CLAUDE.md`, under **Build-graph guardrails**, add this bullet after the "Ratchet lists only shrink." bullet:

```markdown
- **Warnings are errors in first-party code.** `REPO.bazel` enables the toolchain's `treat_warnings_as_errors` feature, so a compiler or linker warning fails the build locally and in CI. Fix the cause: never commit an opt-out (`-treat_warnings_as_errors`), a `#pragma` suppression, or a `-Wno-*` flag. `--features=-treat_warnings_as_errors` on the command line is a local escape hatch for toolchain drift only. See [ADR 0018](docs/adr/0018-treat-first-party-warnings-as-errors.md).
```

- [ ] **Step 4: Verify links and formatting**

Run: `prek run --all-files`
Expected: passes (lychee checks the new links, including the `#error-handling` fragment).

- [ ] **Step 5: Commit**

```bash
git add docs/adr/0018-treat-first-party-warnings-as-errors.md docs/adr/README.md CLAUDE.md
git commit -m "docs: record that first-party warnings are errors (ADR 0018)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J"
```

### Task 14: Prove it in CI and submit the stack

**Files (temporary, reverted before merge):**
- Modify: `src/algorithms/checksum/checksum_ecu_mitsu_m32r_can.cpp`
- Modify: `src/platform/desktop/windows/j2534/j2534_bridge_protocol.cpp`

- [ ] **Step 1: Submit the stack (after the user authorizes pushing)**

```bash
git push -u origin build/warnings-as-errors
gh stack init --base master fix/clear-warnings build/warnings-as-errors
gh stack submit --auto
```

Edit PR 2's description to:

```markdown
Second of two PRs for #48 (stacked on the first). Makes compiler and linker warnings in first-party code fatal.

- `REPO.bazel` requests `treat_warnings_as_errors` for the main repository only; external repositories are unaffected.
- The x86 MSVC toolchain defines the feature itself, so the J2534 bridge binaries are gated too.
- clang-tidy runs with `-Wno-error`; the build is the warnings gate.
- ADR 0018 and a CLAUDE.md rule record the decision. `--features=-treat_warnings_as_errors` is a local-only escape hatch.
- The Android cross-compile is not gated (`rules_android_ndk` lacks the feature); the desktop builds cover the same code.

Closes #48.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01FDf8sLE7SYwb3N3JSM3B5J
```

- [ ] **Step 2: Read the first CI run**

This run recompiles every first-party action (the feature changes their flags), so any warning left on Linux or Windows now fails as an error in the `bazel build -k` step. For each failing job, list the errors:

```bash
gh run list --branch build/warnings-as-errors --limit 1
gh run view <run-id> --log-failed | grep -E "error:|error C[0-9]+|warning C[0-9]+|LNK[0-9]+" | sort -u
```

Fix each one on `fix/clear-warnings` following the task that owns its kind (sign-compare: Task 3 rules, including the whole-file sweep; and so on), then `gh stack rebase` and `gh stack push`. Repeat until the Bazel jobs on all three platforms, SonarCloud, clang-tidy, and pre-commit are green.

- [ ] **Step 3: Negative control**

On `build/warnings-as-errors`, add a commit with these two probes.

At the end of `src/algorithms/checksum/checksum_ecu_mitsu_m32r_can.cpp`:

```cpp
// Negative control for ADR 0018; this commit is dropped before merge.
[[nodiscard]] int warnings_gate_probe();
inline void warnings_gate_probe_caller()
{
    warnings_gate_probe();
}
```

At the end of `src/platform/desktop/windows/j2534/j2534_bridge_protocol.cpp` (compiled by both the x64 and the x86 MSVC toolchains), the same four declarations with the names `bridge_warnings_gate_probe` and `bridge_warnings_gate_probe_caller`.

Discarding a `[[nodiscard]]` result is a warning on all three compilers at today's levels (`-Wunused-result` on clang and GCC, C4834 on MSVC). Push it.
Expected: the macOS, Linux, and Windows Bazel jobs each FAIL on `checksum_ecu_mitsu_m32r_can.cpp`; the Windows job also fails on `j2534_bridge_protocol.cpp` for both the x64 target and the x86 bridge target. The portable-core job is not expected to fail.

Then drop the commit (`git reset --hard HEAD~1`, `git push --force-with-lease`) and confirm CI is green again.

- [ ] **Step 4: Hand off**

Report the two PR links and the stack, the negative-control evidence (job names and their failure lines), and that merging uses `gh stack merge`; `gh stack sync` updates the stack after a merge. Then delete the spec and plan from `docs/superpowers/` in a final commit on PR 1 if the user wants them out of the tree, as earlier designs were.
