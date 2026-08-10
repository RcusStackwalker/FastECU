# OpenPort ISO15765 Flow-Control Configuration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Configure OpenPort ISO15765 receive flow control as `STMIN=0, BS=0`, retry as `STMIN=1, BS=16` when rejected, and fail initialization if both attempts fail.

**Architecture:** Extract timing request/retry logic into a package-local helper accepting a `SET_CONFIG` callback, so behavior is testable without hardware or a vendor DLL. `SerialPortActionsDirect` supplies the callback invoking its existing J2534 object; raw CAN uses the helper's loopback-only path.

**Tech Stack:** C++23, Qt 6, J2534 `SCONFIG`/`SCONFIG_LIST`, QtTest, Bazel.

## Global Constraints

- First ISO15765 request: exactly `LOOPBACK=0`, `ISO15765_STMIN=0`, `ISO15765_BS=0`.
- Only compatibility retry: exactly `LOOPBACK=0`, `ISO15765_STMIN=1`, `ISO15765_BS=16`.
- A second rejection returns `STATUS_ERROR` from `set_j2534_can_timings()`.
- Raw CAN retains one loopback-only request and never uses ISO15765 parameters.
- Add no preference, adapter detection, backend dependency, or unrelated refactor.

---

### Task 1: Test and implement ISO15765 timing retries

**Files:**
- Create: `src/platform/desktop/common/serial/j2534_can_timing_config.h`
- Create: `src/platform/desktop/common/serial/j2534_can_timing_config.cpp`
- Create: `src/platform/desktop/common/serial/j2534_can_timing_config_test.cpp`
- Modify: `src/platform/desktop/common/serial/BUILD.bazel`
- Modify: `src/platform/desktop/common/serial/serial_port_actions_direct.cpp:1504`

**Interfaces:**
- Consumes: platform-selected J2534 definitions for `SCONFIG`, `SCONFIG_LIST`, `LOOPBACK`, `ISO15765_STMIN`, and `ISO15765_BS`.
- Produces: `bool configureJ2534CanTimings(bool iso15765, const J2534SetConfig& setConfig)`, where `using J2534SetConfig = std::function<long(const SCONFIG_LIST&)>`.

- [ ] **Step 1: Write failing tests for first-request success and raw CAN isolation**

Create the QtTest fixture and use a callback that copies every transient `SCONFIG` into `std::vector<std::vector<SCONFIG>>`:

```cpp
void TestJ2534CanTimingConfig::iso15765_usesFastestFlowControl()
{
    std::vector<std::vector<SCONFIG>> calls;
    const bool configured = configureJ2534CanTimings(
        true, [&calls](const SCONFIG_LIST& list) {
            calls.emplace_back(list.ConfigPtr, list.ConfigPtr + list.NumOfParams);
            return STATUS_NOERROR;
        });

    QVERIFY(configured);
    QCOMPARE(calls.size(), std::size_t{1});
    QCOMPARE(calls[0].size(), std::size_t{3});
    QCOMPARE(calls[0][0].Parameter, static_cast<unsigned long>(LOOPBACK));
    QCOMPARE(calls[0][0].Value, 0UL);
    QCOMPARE(calls[0][1].Parameter, static_cast<unsigned long>(ISO15765_STMIN));
    QCOMPARE(calls[0][1].Value, 0UL);
    QCOMPARE(calls[0][2].Parameter, static_cast<unsigned long>(ISO15765_BS));
    QCOMPARE(calls[0][2].Value, 0UL);
}

void TestJ2534CanTimingConfig::rawCan_onlyDisablesLoopback()
{
    std::vector<std::vector<SCONFIG>> calls;
    const bool configured = configureJ2534CanTimings(
        false, [&calls](const SCONFIG_LIST& list) {
            calls.emplace_back(list.ConfigPtr, list.ConfigPtr + list.NumOfParams);
            return STATUS_NOERROR;
        });

    QVERIFY(configured);
    QCOMPARE(calls.size(), std::size_t{1});
    QCOMPARE(calls[0].size(), std::size_t{1});
    QCOMPARE(calls[0][0].Parameter, static_cast<unsigned long>(LOOPBACK));
    QCOMPARE(calls[0][0].Value, 0UL);
}
```

Add `fastecu_qttest(name = "test_j2534_can_timing_config", ...)` and a focused `qt_cc_library(name = "j2534_can_timing_config", ...)` to `BUILD.bazel`, each with the existing platform-selected J2534 dependency.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
bazel test --config=release //src/platform/desktop/common/serial:test_j2534_can_timing_config
```

Expected: FAIL because the header and helper do not exist.

- [ ] **Step 3: Add the minimal helper for successful first attempts**

Declare:

```cpp
#pragma once

#include <functional>
#include <QtGlobal>

#if defined Q_OS_UNIX
#include "src/platform/desktop/unix/j2534/J2534_tactrix_unix.h"
#elif defined Q_OS_WIN32
#include "src/platform/desktop/windows/j2534/J2534_tactrix_win.h"
#endif

using J2534SetConfig = std::function<long(const SCONFIG_LIST&)>;
bool configureJ2534CanTimings(bool iso15765, const J2534SetConfig& setConfig);
```

Implement the first request:

```cpp
#include "j2534_can_timing_config.h"

#include <array>

bool configureJ2534CanTimings(bool iso15765, const J2534SetConfig& setConfig)
{
    std::array<SCONFIG, 3> config{{
        {LOOPBACK, 0},
        {ISO15765_STMIN, 0},
        {ISO15765_BS, 0},
    }};
    SCONFIG_LIST list{
        static_cast<unsigned long>(iso15765 ? config.size() : std::size_t{1}),
        config.data(),
    };
    return setConfig(list) == STATUS_NOERROR;
}
```

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 5: Add failing tests for fallback and terminal failure**

Add a case whose callback returns `ERR_FAILED` then `STATUS_NOERROR`; assert the second captured list has three entries and use individual `QCOMPARE` calls to verify:

```cpp
QCOMPARE(calls[1].size(), std::size_t{3});
QCOMPARE(calls[1][0].Parameter, static_cast<unsigned long>(LOOPBACK));
QCOMPARE(calls[1][0].Value, 0UL);
QCOMPARE(calls[1][1].Parameter, static_cast<unsigned long>(ISO15765_STMIN));
QCOMPARE(calls[1][1].Value, 1UL);
QCOMPARE(calls[1][2].Parameter, static_cast<unsigned long>(ISO15765_BS));
QCOMPARE(calls[1][2].Value, 16UL);
```

Add a callback that always returns `ERR_FAILED`; assert the helper returns false after exactly two ISO15765 calls. Change the raw-CAN case to return `ERR_FAILED`, assert false, and assert exactly one call.

- [ ] **Step 6: Run the focused test and verify RED**

Run the Step 2 command. Expected: FAIL because no fallback exists.

- [ ] **Step 7: Implement the single ISO15765 fallback**

Replace the helper body after constructing `list` with:

```cpp
if (setConfig(list) == STATUS_NOERROR)
{
    return true;
}
if (!iso15765)
{
    return false;
}

config[1].Value = 1;
config[2].Value = 16;
return setConfig(list) == STATUS_NOERROR;
```

- [ ] **Step 8: Run the focused test and verify GREEN**

Run the Step 2 command. Expected: all four behavior cases PASS.

- [ ] **Step 9: Integrate the helper into `SerialPortActionsDirect`**

Replace the local list construction in `set_j2534_can_timings()` with:

```cpp
const bool configured = configureJ2534CanTimings(
    is_iso15765_connection,
    [this](const SCONFIG_LIST& config) {
        return j2534->PassThruIoctl(chanID, SET_CONFIG, &config, nullptr);
    });
if (!configured)
{
    reportJ2534Error();
    return STATUS_ERROR;
}
return STATUS_SUCCESS;
```

Include the helper header, add `:j2534_can_timing_config` to `serial_qt_compat`, and preserve existing log messages.

- [ ] **Step 10: Run package and integration regressions**

```bash
bazel test --config=release //src/platform/desktop/common/serial:all //tests:tst_serial_port_crash
```

Expected: all compatible tests PASS; incompatible platform targets are skipped.

- [ ] **Step 11: Run formatting and diff validation**

```bash
prek run --all-files
git diff --check
```

Expected: both commands exit 0.

- [ ] **Step 12: Commit the implementation**

```bash
git add src/platform/desktop/common/serial/BUILD.bazel \
  src/platform/desktop/common/serial/j2534_can_timing_config.h \
  src/platform/desktop/common/serial/j2534_can_timing_config.cpp \
  src/platform/desktop/common/serial/j2534_can_timing_config_test.cpp \
  src/platform/desktop/common/serial/serial_port_actions_direct.cpp
git commit -m "perf: tune ISO15765 flow control"
```
