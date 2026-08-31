# Consolidate duplicated `cks_add8` checksum Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace 9 copy-pasted (8 of them dead) private `cks_add8` member functions with one tested `FlashUtils::cks_add8` free function.

**Architecture:** Add `std::uint8_t FlashUtils::cks_add8(std::span<const std::uint8_t> data)` to the existing `modules/flash_utils.{h,cpp}` shared-utility module (already `#include`d everywhere it's needed). Point the two real call sites (both in `ecu_operations.cpp`) at it. Delete the 8 other duplicate/dead copies outright — all are `private` and unused, so no caller anywhere is affected.

**Tech Stack:** C++20 (`std::span`), Qt (`QByteArray`), QTest (existing `tests/test_flash_utils.cpp`), Bazel (`bazel test //tests:test_flash_utils`).

## Global Constraints

- C++20 is available and used project-wide (`.bazelrc`: `-std=c++20` / `/std:c++20` on all platforms) — `std::span` is safe to use.
- No new files are created and no file is added to or removed from any source list, so `bazel/fastecu_sources.bzl` and `FastECU.pro` are **not** touched, and `bazel test //:qmake_bazel_sync` must still pass unmodified.
- All 9 existing `cks_add8` declarations are `private` — confirmed via `awk` scan of every header — so deleting the 8 unused ones cannot break any caller.
- Behavior of the 2 live call sites (both in `ecu_operations.cpp`, both `cks_add8(chksum_data, 131)`) must be bit-for-bit unchanged.
- The single `//tests:mut_dma_tests` Bazel target no longer exists on `master` — a same-day commit (`51916cc`, merged just before this plan's branch was rebased) split it into one `qt_cc_test` per suite via `bazel/mut_dma_test_suites.bzl`. The suite for `tests/test_flash_utils.cpp` is now its own target: `//tests:test_flash_utils`. It links the full `//:fastecu_core_common` library (which includes every file this plan touches: `ecu_operations.cpp` and all 8 duplicate-copy files), so a passing build+link of this one target proves the whole shared library still compiles even though it doesn't execute the other classes' code paths.
- Baseline (this branch, rebased onto `origin/master` at commit `51916cc`, no further changes): `bazel test //tests:test_flash_utils --test_output=summary` → `PASSED in 0.8s`, `Executed 1 out of 1 test: 1 test passes.` Every task must end with this command still passing.

---

### Task 1: Add `FlashUtils::cks_add8` with tests

**Files:**
- Modify: `modules/flash_utils.h`
- Modify: `modules/flash_utils.cpp`
- Test: `tests/test_flash_utils.cpp`

**Interfaces:**
- Produces: `std::uint8_t FlashUtils::cks_add8(std::span<const std::uint8_t> data)` — one's-complement-style 8-bit checksum (sum bytes, add 1 on carry out of bit 8, keep low 8 bits). Later tasks (2 and 3) call this exact signature.

- [ ] **Step 1: Write the failing tests**

Open `tests/test_flash_utils.cpp`. First, change the includes:

```cpp
#include <QtTest>

#include "modules/flash_utils.h"
#include "serial_port_actions.h"
#include "fake_backend.h"
#include "test_flash_utils.h"
```

to:

```cpp
#include <QtTest>

#include <array>
#include <cstdint>
#include <span>

#include "modules/flash_utils.h"
#include "serial_port_actions.h"
#include "fake_backend.h"
#include "test_flash_utils.h"
```

Then add four new `private slots` methods to `TestFlashUtils`, right after
`configureIso15765Can_defaultsTo11BitCanIds()`'s closing `}` and before the
class's closing `};`:

```cpp
    void cksAdd8_returnsZeroForEmptyData()
    {
        QCOMPARE(FlashUtils::cks_add8(std::span<const std::uint8_t>{}), std::uint8_t(0));
    }

    void cksAdd8_sumsBytesWithoutCarry()
    {
        const std::array<std::uint8_t, 3> bytes{0x01, 0x02, 0x03};
        QCOMPARE(FlashUtils::cks_add8(std::span<const std::uint8_t>(bytes)), std::uint8_t(6));
    }

    void cksAdd8_addsOneOnCarry()
    {
        // Plain mod-256 truncation of 0xFF + 0xFF would give 0xFE; the
        // "add 1 on carry" step this checksum is named for makes it 0xFF.
        const std::array<std::uint8_t, 2> bytes{0xFF, 0xFF};
        QCOMPARE(FlashUtils::cks_add8(std::span<const std::uint8_t>(bytes)), std::uint8_t(0xFF));
    }

    void cksAdd8_matchesReflashBlockShape()
    {
        // Matches EcuOperations::npk_raw_flashblock's real call shape: a
        // 131-byte block (3-byte address header + 128-byte payload).
        // Repeated carry corrections over 131 additions of 0x02 give 7,
        // not the naive mod-256 sum of 131*2 = 262 -> 6.
        const std::array<std::uint8_t, 131> bytes = [] {
            std::array<std::uint8_t, 131> data{};
            data.fill(0x02);
            return data;
        }();
        QCOMPARE(FlashUtils::cks_add8(std::span<const std::uint8_t>(bytes)), std::uint8_t(7));
    }
```

- [ ] **Step 2: Run the tests to verify they fail to compile**

Run: `bazel test //tests:test_flash_utils --test_output=errors`
Expected: FAIL — compile error, `no member named 'cks_add8' in namespace 'FlashUtils'`.

- [ ] **Step 3: Add the declaration**

In `modules/flash_utils.h`, change:

```cpp
#include <QByteArray>
#include <QString>

#include <kernelmemorymodels.h>

class SerialPortActions;

namespace FlashUtils
{
int findFlashDeviceIndex(const QString& mcuType);
const flashdev_t *findFlashDevice(const QString& mcuType);
```

to:

```cpp
#include <QByteArray>
#include <QString>

#include <cstdint>
#include <span>

#include <kernelmemorymodels.h>

class SerialPortActions;

namespace FlashUtils
{
int findFlashDeviceIndex(const QString& mcuType);
const flashdev_t *findFlashDevice(const QString& mcuType);

// One's-complement-style 8-bit checksum: sum bytes, and whenever the
// running sum overflows 8 bits, add 1 back in before truncating (rather
// than a plain mod-256 sum). Used to checksum ECU reflash blocks.
std::uint8_t cks_add8(std::span<const std::uint8_t> data);
```

- [ ] **Step 4: Add the definition**

In `modules/flash_utils.cpp`, add this function as the first function in the `namespace FlashUtils` block, immediately after the opening `namespace FlashUtils\n{` line and before `int findFlashDeviceIndex(const QString& mcuType)`:

```cpp
std::uint8_t cks_add8(std::span<const std::uint8_t> data)
{
    std::uint16_t sum = 0;
    for (std::uint8_t byte : data)
    {
        sum += byte;
        if (sum & 0x100)
        {
            sum += 1;
        }
        sum = static_cast<std::uint8_t>(sum);
    }
    return static_cast<std::uint8_t>(sum);
}

```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bazel test //tests:test_flash_utils --test_output=errors`
Expected: `//tests:test_flash_utils PASSED`, `Executed 1 out of 1 test: 1 test passes.`

- [ ] **Step 6: Commit**

```bash
git add modules/flash_utils.h modules/flash_utils.cpp tests/test_flash_utils.cpp
git commit -m "feat: add FlashUtils::cks_add8 shared checksum helper"
```

---

### Task 2: Point `EcuOperations` at `FlashUtils::cks_add8` and remove its duplicate

**Files:**
- Modify: `ecu_operations.h:93`
- Modify: `ecu_operations.cpp:1,1249,1366,2074-2091`

**Interfaces:**
- Consumes: `std::uint8_t FlashUtils::cks_add8(std::span<const std::uint8_t> data)` from Task 1.

- [ ] **Step 1: Add the include**

In `ecu_operations.cpp`, change:

```cpp
#include "ecu_operations.h"
#include "serial_port_actions.h"
```

to:

```cpp
#include "ecu_operations.h"
#include "modules/flash_utils.h"
#include "serial_port_actions.h"
```

- [ ] **Step 2: Replace both call sites**

There are two identical occurrences of this exact block in `ecu_operations.cpp` (one around line 1249, one around line 1366) — use `replace_all` (both are the literal string `cks_add8(chksum_data, 131)`):

Change:

```cpp
        chksum_data.append(cks_add8(chksum_data, 131));
```

to:

```cpp
        chksum_data.append(FlashUtils::cks_add8(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(chksum_data.constData()), 131)));
```

(both occurrences are identical, so a single `replace_all` edit covers both).

- [ ] **Step 3: Delete the declaration**

In `ecu_operations.h`, remove this line:

```cpp
    uint8_t cks_add8(QByteArray chksum_data, unsigned len);
```

(it sits between `void set_progressbar_value(int value);` above and `void init_crc16_tab(void);` below — delete only the `cks_add8` line, keep its neighbors).

- [ ] **Step 4: Delete the definition**

In `ecu_operations.cpp`, remove this block (the comment and the function together; leave the surrounding blank lines as they already separate this from the `crc16` block below it):

```cpp
/* special checksum for reflash blocks:
 * "one's complement" checksum; if adding causes a carry, add 1 to sum. Slightly better than simple 8bit sum
 */
uint8_t EcuOperations::cks_add8(QByteArray chksum_data, unsigned len)
{
    uint16_t sum = 0;

    for (unsigned i = 0; i < len; i++)
    {
        sum += static_cast<uint8_t>(chksum_data[i]);
        if (sum & 0x100)
        {
            sum += 1;
        }
        sum = (uint8_t)sum;
    }
    return sum;
}

```

- [ ] **Step 5: Run the full test suite**

Run: `bazel test //tests:test_flash_utils --test_output=errors`
Expected: `//tests:test_flash_utils PASSED`, `Executed 1 out of 1 test: 1 test passes.`

- [ ] **Step 6: Commit**

```bash
git add ecu_operations.h ecu_operations.cpp
git commit -m "refactor: EcuOperations uses FlashUtils::cks_add8"
```

---

### Task 3: Delete the 8 dead duplicate copies

**Files:**
- Modify: `modules/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.h`, `.cpp`
- Modify: `modules/ecu/flash_ecu_subaru_denso_sh7058_can_operation.h`, `.cpp`
- Modify: `modules/tcu/flash_tcu_subaru_denso_sh705x_can_operation.h`, `.cpp`
- Modify: `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline_operation.h`, `.cpp`
- Modify: `modules/ecu/flash_ecu_subaru_denso_sh7058_can_diesel_operation.h`, `.cpp`
- Modify: `modules/ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.h`, `.cpp`
- Modify: `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_can_operation.h`, `.cpp`
- Modify: `modules/ecu/flash_ecu_subaru_denso_sh7055_02_operation.h` (declaration only — no `.cpp` definition exists for this one)

**Interfaces:**
- None — all 8 removed methods are `private` and confirmed (by grep across every `.cpp` in scope) to have zero call sites anywhere in their own class.

For each of the first 7 classes, remove one header declaration line and one `.cpp` comment+function block. The 8th class only needs the header line removed.

- [ ] **Step 1: `flash_ecu_subaru_denso_sh705x_kline_operation`**

In `modules/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.h`, delete:

```cpp
    uint8_t cks_add8(QByteArray chksum_data, unsigned len);
```

(the line directly after `int reflash_block(const uint8_t *newdata, const struct flashdev_t *fdt, unsigned blockno, bool test_write);`).

In `modules/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp`, delete:

```cpp
/*
 * 8bit checksum
 *
 * @return
 */
uint8_t FlashEcuSubaruDensoSH705xKlineOperation::cks_add8(QByteArray chksum_data, unsigned len)
{
    uint16_t sum = 0;
    for (unsigned i = 0; i < len; i++)
    {
        sum += (uint8_t)chksum_data.at(i); // data[i];
        if (sum & 0x100)
        {
            sum += 1;
        }
        sum = (uint8_t)sum;
    }
    return sum;
}

```

(this sits between the end of `npk_raw_flashblock_16bit_kline`'s enclosing function — the line above is `return STATUS_SUCCESS;\n}` — and the `/* ECU init ...` comment above `send_sid_bf_ssm_init`; leave one blank line where this block was, i.e. delete through the function's closing `}` and the one blank line right after it, since a blank line already precedes the block too. Note: an unrelated same-day commit already removed this class's variable-length-array line and its commented-out fill loop as part of a Windows UB fix — the block above reflects that current state.)

- [ ] **Step 2: `flash_ecu_subaru_denso_sh7058_can_operation`**

In `modules/ecu/flash_ecu_subaru_denso_sh7058_can_operation.h`, delete:

```cpp
    uint8_t cks_add8(QByteArray chksum_data, unsigned len);
```

In `modules/ecu/flash_ecu_subaru_denso_sh7058_can_operation.cpp`, delete:

```cpp
/*
 * 8bit checksum
 *
 * @return
 */
uint8_t FlashEcuSubaruDensoSH7058CanOperation::cks_add8(QByteArray chksum_data, unsigned len)
{
    uint16_t sum = 0;
    for (unsigned i = 0; i < len; i++)
    {
        sum += (uint8_t)chksum_data.at(i);
        if (sum & 0x100)
            sum += 1;
        sum = (uint8_t)sum;
    }
    return sum;
}

```

(note: an unrelated same-day commit already removed this class's variable-length-array
line (`uint8_t data[chksum_data.length()];`) as part of a Windows UB fix — the block
above reflects that current state, not the original copy-pasted form.)

- [ ] **Step 3: `flash_tcu_subaru_denso_sh705x_can_operation`**

In `modules/tcu/flash_tcu_subaru_denso_sh705x_can_operation.h`, delete:

```cpp
    uint8_t cks_add8(QByteArray chksum_data, unsigned len);
```

In `modules/tcu/flash_tcu_subaru_denso_sh705x_can_operation.cpp`, delete:

```cpp
/*
 * 8bit checksum
 *
 * @return
 */
uint8_t FlashTcuSubaruDensoSH705xCanOperation::cks_add8(QByteArray chksum_data, unsigned len)
{
    uint16_t sum = 0;
    for (unsigned i = 0; i < len; i++)
    {
        sum += (uint8_t)chksum_data.at(i); // data[i];
        if (sum & 0x100)
        {
            sum += 1;
        }
        sum = (uint8_t)sum;
    }
    return sum;
}

```

(note: an unrelated same-day commit already removed this class's variable-length-array
line and its commented-out fill loop as part of a Windows UB fix — the block above
reflects that current state, not the original copy-pasted form.)

- [ ] **Step 4: `eeprom_ecu_subaru_denso_sh705x_kline_operation`**

In `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline_operation.h`, delete:

```cpp
    uint8_t cks_add8(QByteArray chksum_data, unsigned len);
```

(the line directly after the blank line following `int read_mem(uint32_t start_addr, uint32_t length);`).

In `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline_operation.cpp`, delete:

```cpp
/*
 * 8bit checksum
 *
 * @return
 */
uint8_t EepromEcuSubaruDensoSH705xKlineOperation::cks_add8(QByteArray chksum_data, unsigned len)
{
    uint16_t sum = 0;
    for (unsigned i = 0; i < len; i++)
    {
        sum += (uint8_t)chksum_data.at(i); // data[i];
        if (sum & 0x100)
        {
            sum += 1;
        }
        sum = (uint8_t)sum;
    }
    return sum;
}

```

(note: an unrelated same-day commit already removed this class's variable-length-array
line and its commented-out fill loop as part of a Windows UB fix — the block above
reflects that current state, not the original copy-pasted form.)

- [ ] **Step 5: `flash_ecu_subaru_denso_sh7058_can_diesel_operation`**

In `modules/ecu/flash_ecu_subaru_denso_sh7058_can_diesel_operation.h`, delete:

```cpp
    uint8_t cks_add8(QByteArray chksum_data, unsigned len);
```

In `modules/ecu/flash_ecu_subaru_denso_sh7058_can_diesel_operation.cpp`, delete:

```cpp
/*
 * 8bit checksum
 *
 * @return
 */
uint8_t FlashEcuSubaruDensoSH7058CanDieselOperation::cks_add8(QByteArray chksum_data, unsigned len)
{
    uint16_t sum = 0;
    for (unsigned i = 0; i < len; i++)
    {
        sum += (uint8_t)chksum_data.at(i);
        if (sum & 0x100)
            sum += 1;
        sum = (uint8_t)sum;
    }
    return sum;
}

```

(note: an unrelated same-day commit already removed this class's variable-length-array
line (`uint8_t data[chksum_data.length()];`) as part of a Windows UB fix — the block
above reflects that current state, not the original copy-pasted form.)

- [ ] **Step 6: `flash_ecu_subaru_denso_sh705x_densocan_operation`**

In `modules/ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.h`, delete:

```cpp
    uint8_t cks_add8(QByteArray chksum_data, unsigned len);
```

In `modules/ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.cpp`, delete:

```cpp
/*
 * 8bit checksum
 *
 * @return
 */
uint8_t FlashEcuSubaruDensoSH705xDensoCanOperation::cks_add8(QByteArray chksum_data, unsigned len)
{
    uint16_t sum = 0;
    for (unsigned i = 0; i < len; i++)
    {
        sum += (uint8_t)chksum_data.at(i);
        if (sum & 0x100)
            sum += 1;
        sum = (uint8_t)sum;
    }
    return sum;
}

```

(note: an unrelated same-day commit already removed this class's variable-length-array
line (`uint8_t data[chksum_data.length()];`) as part of a Windows UB fix — the block
above reflects that current state, not the original copy-pasted form.)

- [ ] **Step 7: `eeprom_ecu_subaru_denso_sh705x_can_operation`**

In `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_can_operation.h`, delete:

```cpp
    uint8_t cks_add8(QByteArray chksum_data, unsigned len);
```

In `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_can_operation.cpp`, delete:

```cpp
/*
 * 8bit checksum
 *
 * @return
 */
uint8_t EepromEcuSubaruDensoSH705xCanOperation::cks_add8(QByteArray chksum_data, unsigned len)
{
    uint16_t sum = 0;
    for (unsigned i = 0; i < len; i++)
    {
        sum += (uint8_t)chksum_data.at(i); // data[i];
        if (sum & 0x100)
        {
            sum += 1;
        }
        sum = (uint8_t)sum;
    }
    return sum;
}

```

(note: an unrelated same-day commit already removed this class's variable-length-array
line and its commented-out fill loop as part of a Windows UB fix — the block above
reflects that current state, not the original copy-pasted form.)

- [ ] **Step 8: `flash_ecu_subaru_denso_sh7055_02_operation` (declaration only)**

In `modules/ecu/flash_ecu_subaru_denso_sh7055_02_operation.h`, delete:

```cpp
    uint8_t cks_add8(QByteArray chksum_data, unsigned len);
```

(the line directly after `int reflash_block(const uint8_t *newdata, const struct flashdev_t *fdt, unsigned blockno, bool test_write);`; there is no matching definition in the `.cpp` file to remove — confirmed by grep).

- [ ] **Step 9: Confirm no references remain**

Run: `grep -rn "cks_add8" --include="*.cpp" --include="*.h" . | grep -v bazel- | grep -v ".claude/worktrees"`
Expected output: exactly 3 lines, all in `modules/flash_utils.h`, `modules/flash_utils.cpp`, and `tests/test_flash_utils.cpp` (the Task 1 additions), plus the two `FlashUtils::cks_add8(` call sites in `ecu_operations.cpp`. No `ClassName::cks_add8` definitions and no bare `cks_add8(QByteArray` declarations should remain.

- [ ] **Step 10: Run the full test suite**

Run: `bazel test //tests:test_flash_utils --test_output=errors`
Expected: `//tests:test_flash_utils PASSED`, `Executed 1 out of 1 test: 1 test passes.`

- [ ] **Step 11: Run the qmake/Bazel source-list sync check**

Run: `bazel test //:qmake_bazel_sync --test_output=errors`
Expected: PASSED (no source files were added/removed/renamed, so this should be unaffected).

- [ ] **Step 12: Commit**

```bash
git add modules/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.h modules/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp \
        modules/ecu/flash_ecu_subaru_denso_sh7058_can_operation.h modules/ecu/flash_ecu_subaru_denso_sh7058_can_operation.cpp \
        modules/tcu/flash_tcu_subaru_denso_sh705x_can_operation.h modules/tcu/flash_tcu_subaru_denso_sh705x_can_operation.cpp \
        modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline_operation.h modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline_operation.cpp \
        modules/ecu/flash_ecu_subaru_denso_sh7058_can_diesel_operation.h modules/ecu/flash_ecu_subaru_denso_sh7058_can_diesel_operation.cpp \
        modules/ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.h modules/ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.cpp \
        modules/eeprom/eeprom_ecu_subaru_denso_sh705x_can_operation.h modules/eeprom/eeprom_ecu_subaru_denso_sh705x_can_operation.cpp \
        modules/ecu/flash_ecu_subaru_denso_sh7055_02_operation.h
git commit -m "chore: remove dead duplicate cks_add8 copies"
```

---

## Self-Review Notes

- **Spec coverage:** All 4 "Changes" items from the design spec map onto Task 1 (item 1 + 4's test additions), Task 2 (item 2), Task 3 (items 3 covering all 8 files including the declaration-only one). The spec's "Out of scope" items are untouched by any task.
- **Type consistency:** `std::uint8_t FlashUtils::cks_add8(std::span<const std::uint8_t> data)` is declared once in Task 1 Step 3 and used identically in Task 1's tests, Task 2 Step 2, matching the spec's signature exactly.
- **No placeholders:** every step shows exact code/diffs; no "similar to above" shortcuts — each of the 7 near-identical file pairs in Task 3 is spelled out in full because an implementer may work the steps out of order.
