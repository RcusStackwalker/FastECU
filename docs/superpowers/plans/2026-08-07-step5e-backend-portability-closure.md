# Step 5e — Backend Portability Closure — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `src/backend` provably portable — zero `//src/backend/...` entries in the `serial_qt_compat` frozen allowlist, and no `QT_DEPS` in `//src/backend/flash` or `//src/backend/checksum`.

**Architecture:** Three independent items. Retire one stale allowlist entry; decompose `backend/flash/flash_utils` so its Qt/serial half moves to the platform package that already owns its callers and its portable half merges with an existing duplicate; delete `LegacyChecksumAdapter` and `FileActions::checksum_correction`, hoisting the checksum dialogs into a new `src/ui/desktop/checksum/` package.

**Tech Stack:** C++23, Bazel 9.1.1, Qt 6.8.3, GoogleTest 1.17.0.bcr.2, QtTest. Build via `bazel`, lint via `prek`.

**Design doc:** [step 5e design](../specs/2026-08-07-step5e-backend-portability-closure-design.md)

## Global Constraints

- **Branch, never `master`.** A `no-commit-to-branch` prek hook rejects commits to `master`. Work on a branch; this plan's work continues on `docs/step5e-backend-portability-closure-design` or a fresh branch off it.
- **Every header needs `#pragma once`** (enforced by prek).
- **Qt targets list moc'd headers explicitly in `MOC_HDRS`**, everything else in `normal_hdrs`. A `Q_OBJECT` header missing from `MOC_HDRS` links but fails at runtime.
- **New packages use explicit `srcs`**, never `glob(["*.cpp"])`. `src/ui/desktop`'s glob has no `*_test.cpp` exclusion; a co-located test there would link into the production library.
- **Prefer `std::string_view` by value** over `const char*` / `const std::string&` (ADR 0009); **`std::format`** for message construction (ADR 0011).
- **Backend operations return `fastecu::Result<T>`**; exceptions never cross a port.
- **Byte types:** `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView` from `src/algorithms/protocol/bytes.h`. `QByteArray` is a boundary type only, converted via `qt_bytes.h` (ADR 0004).
- **Cross-document Markdown references are links with human-readable text**, not backticked paths — lychee cannot see a path written as inline code.
- **Full gate**, run before each commit that changes BUILD files or C++:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `src/platform/desktop/common/flash/legacy/legacy_flash_utils.h` | Qt-typed `FlashUtils::` shim: `configureIso15765Can` (real impl) + `findFlashDeviceIndex` (forwards to portable) |
| `src/platform/desktop/common/flash/legacy/legacy_flash_utils.cpp` | Their implementations |
| `src/platform/desktop/common/flash/legacy/legacy_flash_utils_test.cpp` | The two `configureIso15765Can` cases, as QtTest |
| `src/backend/flash/flash_device_lookup.h` | Portable `find_flash_device` + `find_flash_device_index` on `std::string_view` |
| `src/backend/flash/flash_device_lookup.cpp` | Their implementations |
| `src/backend/flash/flash_device_lookup_test.cpp` | Merged assertions from both predecessor suites |
| `src/ui/desktop/checksum/BUILD.bazel` | Explicit-`srcs` package for the checksum command |
| `src/ui/desktop/checksum/checksum_correction_command.h` | `ChecksumCorrectionCommand`: dialogs + sequencing, four `virtual` seams |
| `src/ui/desktop/checksum/checksum_correction_command.cpp` | Its implementation |
| `src/ui/desktop/checksum/checksum_correction_command_test.cpp` | Converted adapter suite + two recovered `FileActions` cases |

**Deleted:**

| File / target | Reason |
|---|---|
| `src/backend/flash/flash_utils.{h,cpp}`, `flash_utils_test.cpp` | Decomposed across four destinations |
| `//src/backend/flash:flash`, `//src/backend/flash:test_flash_utils` | The Qt target and its test |
| `src/backend/checksum/flash_device_lookup.{h,cpp}`, `flash_device_lookup_test.cpp` | Moved to `//src/backend/flash` |
| `src/backend/checksum/legacy_checksum_adapter.{h,cpp}`, `legacy_checksum_adapter_test.cpp` | Hoisted to UI |
| `//src/backend/checksum:legacy_checksum_adapter`, `:test_legacy_checksum_adapter` | Same |
| `FileActions::checksum_correction`, `checksumAdapter_` | Hoisted to UI |

**Modified:** `src/algorithms/checksum/checksum_primitives.{h,cpp}` (gains `cks_add8`) and its test; `src/backend/checksum/BUILD.bazel`; `src/backend/flash/BUILD.bazel`; `src/backend/checksum/dispatch.{h,cpp}`; `src/backend/definitions/file_actions.{h,cpp}`; `src/backend/definitions/file_actions_parsing_test.cpp`; `src/ui/desktop/mainwindow.{h,cpp}`; `src/ui/desktop/menu_actions.cpp`; `src/ui/desktop/ecu_operations.cpp`; `src/ui/desktop/BUILD.bazel`; `src/ui/desktop/flash/ecu/BUILD.bazel`; `src/platform/desktop/common/flash/legacy/BUILD.bazel` + its 27 operation `.cpp` files (include line only); `src/platform/desktop/common/flash/legacy/flash_ecu_mitsu_m32r_can_operation.{cpp,_test.cpp}`; `scripts/check-portable-closure.py`; `scripts/check-serial-compat-allowlist.py`; `docs/tech-debt.md`; `docs/modularization-plan.md`; `docs/superpowers/specs/2026-07-24-step5d-fileactions-decomposition-design.md`.

---

## Task 1: Retire the stale `logging/protocols` allowlist entry

This is 5e-3. It lands first and alone so the freeze test's behavior is exercised before Task 3 edits the same file.

**Files:**
- Modify: `scripts/check-serial-compat-allowlist.py:33`
- Modify: `src/platform/desktop/common/serial/BUILD.bazel` (the `serial_qt_compat` `visibility` list)

**Interfaces:**
- Consumes: nothing.
- Produces: a `FROZEN` set of 13 entries, one of which (`//src/backend/flash:__pkg__`) Task 3 removes.

- [ ] **Step 1: Prove the entry is unused before removing it**

`//src/backend/logging/protocols` should have neither a `serial_qt_compat` dep nor `QT_DEPS`. Confirm, and confirm it is already registered portable:

```bash
grep -n "serial\|QT_DEPS" src/backend/logging/protocols/BUILD.bazel
grep -rn "serial_port_actions" src/backend/logging/
bazel query 'somepath(//src/backend/logging/protocols:protocols, //src/platform/desktop/common/serial:serial_qt_compat)'
```

Expected: first two print nothing; the `bazel query` prints nothing (no path exists).

- [ ] **Step 2: Remove the visibility entry**

In `src/platform/desktop/common/serial/BUILD.bazel`, delete this line from the `serial_qt_compat` `visibility` list:

```python
        "//src/backend/logging/protocols:__pkg__",
```

- [ ] **Step 3: Run the freeze test — it must still pass**

The check only fails on *growth* (`added = actual - FROZEN`), so removing a `visibility` entry while `FROZEN` still lists it passes. That is the intended asymmetry; confirm it:

Run: `bazel test --config=release //:serial_compat_allowlist`
Expected: PASS.

- [ ] **Step 4: Shrink `FROZEN` to match**

In `scripts/check-serial-compat-allowlist.py`, delete this line from the `FROZEN` set:

```python
    "//src/backend/logging/protocols:__pkg__",
```

- [ ] **Step 5: Verify non-vacuity — the check must fail if the entry comes back**

Temporarily re-add `"//src/backend/logging/protocols:__pkg__"` to the BUILD `visibility` list only (not to `FROZEN`), then:

Run: `bazel test --config=release //:serial_compat_allowlist`
Expected: FAIL, printing `FAIL: serial_qt_compat allowlist grew. New entries:` followed by `//src/backend/logging/protocols:__pkg__`.

Then remove it again and re-run; expected PASS. This proves the check is not vacuous.

- [ ] **Step 6: Full gate**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Expected: all pass. The build proves nothing depended on the removed visibility edge.

- [ ] **Step 7: Commit**

```bash
git add scripts/check-serial-compat-allowlist.py src/platform/desktop/common/serial/BUILD.bazel
git commit -m "chore: drop stale serial_qt_compat entry for backend/logging/protocols (5e-3)

That package has no serial_qt_compat dependency and no QT_DEPS, and is
already registered in PORTABLE_ROOTS -- the allowlist entry was left over
from the pre-5b package layout. Verified non-vacuous: re-adding the
visibility entry fails //:serial_compat_allowlist.

Allowlist: 14 entries -> 13."
```

---

## Task 2: Move `cks_add8` to `//src/algorithms/checksum`

**Files:**
- Modify: `src/algorithms/checksum/checksum_primitives.h`
- Modify: `src/algorithms/checksum/checksum_primitives.cpp`
- Modify: `src/algorithms/checksum/checksum_primitives_test.cpp`
- Modify: `src/ui/desktop/ecu_operations.cpp:1289`, `:1407`
- Modify: `src/ui/desktop/BUILD.bazel`

**Interfaces:**
- Consumes: nothing.
- Produces: `std::uint8_t fastecu::checksum::cks_add8(std::span<const std::uint8_t> data)`, declared in `src/algorithms/checksum/checksum_primitives.h`. Task 5 relies on this symbol existing so `flash_utils.cpp` can be deleted.

Note `//src/algorithms/checksum`'s BUILD uses `glob(["*.cpp"], exclude=["*_test.cpp","qt_*.cpp"])` and a matching `hdrs` glob, so no BUILD `srcs` edit is needed — the existing files are already members.

`cks_add8` goes in namespace `fastecu::checksum`, **not** `fastecu::checksum::internal` where `rebalanceU16Be`/`rebalanceU32Be` live: it has an out-of-package caller (`ui/desktop/ecu_operations.cpp`), so it is public API of this package. The name stays `cks_add8` — it is the domain term and keeps the two call sites recognizable.

- [ ] **Step 1: Write the failing tests**

Append to `src/algorithms/checksum/checksum_primitives_test.cpp`. These are the four `cksAdd8_*` cases from `src/backend/flash/flash_utils_test.cpp:180-212`, converted from QtTest to GoogleTest with their comments preserved:

```cpp
TEST(CksAdd8, ReturnsZeroForEmptyData)
{
    EXPECT_EQ(fastecu::checksum::cks_add8(std::span<const std::uint8_t>{}), std::uint8_t(0));
}

TEST(CksAdd8, SumsBytesWithoutCarry)
{
    const std::array<std::uint8_t, 3> data{0x01, 0x02, 0x03};
    EXPECT_EQ(fastecu::checksum::cks_add8(std::span<const std::uint8_t>(data)), std::uint8_t(6));
}

TEST(CksAdd8, AddsOneOnCarry)
{
    // Plain mod-256 truncation of 0xFF + 0xFF would give 0xFE; the
    // "add 1 on carry" step this checksum is named for makes it 0xFF.
    const std::array<std::uint8_t, 2> data{0xFF, 0xFF};
    EXPECT_EQ(fastecu::checksum::cks_add8(std::span<const std::uint8_t>(data)), std::uint8_t(0xFF));
}

TEST(CksAdd8, MatchesReflashBlockShape)
{
    // Matches EcuOperations::npk_raw_flashblock's real call shape: a
    // 131-byte block (3-byte address header + 128-byte payload).
    // Repeated carry corrections over 131 additions of 0x02 give 7,
    // not the naive mod-256 sum of 131*2 = 262 -> 6.
    std::array<std::uint8_t, 131> data{};
    data.fill(0x02);
    EXPECT_EQ(fastecu::checksum::cks_add8(std::span<const std::uint8_t>(data)), std::uint8_t(7));
}
```

Add to that file's includes if absent:

```cpp
#include <array>
#include <cstdint>
#include <span>
```

- [ ] **Step 2: Run to verify it fails**

Run: `bazel test --config=release //src/algorithms/checksum:checksum_primitives_test`
Expected: FAIL — compile error, `cks_add8` is not a member of `fastecu::checksum`.

- [ ] **Step 3: Declare and implement**

Add to `src/algorithms/checksum/checksum_primitives.h`, **outside** the `internal` namespace:

```cpp
namespace fastecu::checksum
{

// One's-complement-style 8-bit checksum: sum bytes, and whenever the
// running sum overflows 8 bits, add 1 back in before truncating (rather
// than a plain mod-256 sum). Used to checksum ECU reflash blocks.
std::uint8_t cks_add8(std::span<const std::uint8_t> data);

} // namespace fastecu::checksum
```

and add `#include <span>` to that header.

Add to `src/algorithms/checksum/checksum_primitives.cpp` (body copied verbatim from `src/backend/flash/flash_utils.cpp:7-20`):

```cpp
namespace fastecu::checksum
{

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

} // namespace fastecu::checksum
```

- [ ] **Step 4: Run to verify it passes**

Run: `bazel test --config=release //src/algorithms/checksum:checksum_primitives_test`
Expected: PASS, 4 new cases green.

- [ ] **Step 5: Re-point the two production call sites**

In `src/ui/desktop/ecu_operations.cpp` at lines 1289 and 1407, change `FlashUtils::cks_add8` to `fastecu::checksum::cks_add8`. Both call sites are otherwise identical and unchanged:

```cpp
        chksum_data.append(fastecu::checksum::cks_add8(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(chksum_data.constData()), 131)));
```

Add to that file's includes:

```cpp
#include "src/algorithms/checksum/checksum_primitives.h"
```

- [ ] **Step 6: Add the dep**

In `src/ui/desktop/BUILD.bazel`, add to the `deps` of the target that compiles `ecu_operations.cpp` (the one already listing `//src/backend/flash` at line 81):

```python
        "//src/algorithms/checksum",
```

Leave `//src/backend/flash` in place for now — Task 5 removes it.

- [ ] **Step 7: Full gate**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Expected: all pass. `//src/backend/flash:test_flash_utils` still passes — its `cksAdd8_*` cases remain and still call `FlashUtils::cks_add8`, which still exists. Both copies coexist until Task 5.

- [ ] **Step 8: Commit**

```bash
git add src/algorithms/checksum/ src/ui/desktop/ecu_operations.cpp src/ui/desktop/BUILD.bazel
git commit -m "refactor(checksum): move cks_add8 to algorithms (5e-1)

A pure reflash-block checksum primitive living in backend/flash behind a
Qt target. Public (not internal) namespace because ui/desktop/ecu_operations
calls it. The FlashUtils copy stays until flash_utils is decomposed."
```

---

## Task 3: Move the portable flash-device lookup to `//src/backend/flash`

Collapses the `backend/checksum` / `FlashUtils` duplication into one portable target and gives it the index-returning form the 30 flash callers need.

**Files:**
- Create: `src/backend/flash/flash_device_lookup.h`
- Create: `src/backend/flash/flash_device_lookup.cpp`
- Create: `src/backend/flash/flash_device_lookup_test.cpp`
- Delete: `src/backend/checksum/flash_device_lookup.{h,cpp}`, `src/backend/checksum/flash_device_lookup_test.cpp`
- Modify: `src/backend/flash/BUILD.bazel`, `src/backend/checksum/BUILD.bazel`
- Modify: `src/backend/checksum/dispatch.{h,cpp}`
- Modify: `scripts/check-portable-closure.py`

**Interfaces:**
- Consumes: nothing.
- Produces, in namespace `fastecu::flash`, declared in `src/backend/flash/flash_device_lookup.h`:
  - `const flashdev_t *find_flash_device(std::string_view mcu_type)`
  - `int find_flash_device_index(std::string_view mcu_type)` — returns `-1` when unknown

  Task 4 forwards to `find_flash_device_index`; Task 8's `ChecksumCorrectionCommand` calls `find_flash_device`.

- [ ] **Step 1: Verify no dependency cycle before moving**

`//src/backend/checksum:dispatch` will depend on `//src/backend/flash:flash_device_lookup`. Confirm `//src/backend/flash`'s portable targets do not already depend on `//src/backend/checksum`:

```bash
bazel query 'somepath(//src/backend/flash:flash_executor, //src/backend/checksum:dispatch)'
bazel query 'somepath(//src/backend/flash:flash_plan, //src/backend/checksum:dispatch)'
```

Expected: both print nothing. If either prints a path, stop — the lookup must stay in `backend/checksum` and this task's design note about `models` applies instead.

- [ ] **Step 2: Write the failing test**

Create `src/backend/flash/flash_device_lookup_test.cpp`. This **merges both predecessor suites** — the `string_view` cases from `src/backend/checksum/flash_device_lookup_test.cpp` and the three `find*` cases from `src/backend/flash/flash_utils_test.cpp:15-134`. Keep every assertion from both; they were written against the same table from opposite sides.

```cpp
#include "src/backend/flash/flash_device_lookup.h"

#include <cstdint>
#include <gtest/gtest.h>

using fastecu::flash::find_flash_device;
using fastecu::flash::find_flash_device_index;

TEST(FindFlashDevice, ReturnsDeviceForEveryMcuStringInShippedProtocolsCfg)
{
    // Every distinct <mcu> value in resources/shared/config/protocols.cfg as
    // of this writing (grep -oP '(?<=<mcu>)[^<]*' resources/shared/config/protocols.cfg
    // | sort -u), except M32170, which is not registered in flashdevices[]
    // -- exercised separately below since checksum correction's "Unknown MCU
    // type" path is not hypothetical, it fires for that real, currently
    // shipped protocol.
    static constexpr const char *kKnown[] = {
        "M32R_128KB",        "M32R_256KB",    "M32R_384KB",   "M32R_384KB_1block",
        "M32R_512KB",        "M32R_512KB_1block", "M32R_512KB_4blocks", "M3775x",
        "M3779x",            "MC68HC16Y5",    "MC68HC16Y5_TPU", "MH8104",
        "MH8111",            "N83M_1_5MB",    "N83M_4MB",     "SH7055",
        "SH7058",            "SH7058_1block", "SH7058d",      "SH7059d",
        "SH72531",           "SH72543d",      "SH72543R",
    };
    for (const char *name : kKnown)
    {
        EXPECT_NE(find_flash_device(name), nullptr) << name;
        EXPECT_GE(find_flash_device_index(name), 0) << name;
    }
}

TEST(FindFlashDevice, ReturnsNullForUnknownMcuType)
{
    // "M32170" is sub_ecu_mitsu_m32r_can's real, currently shipped <mcu>
    // value in protocols.cfg; it is not registered in flashdevices[].
    EXPECT_EQ(find_flash_device("M32170"), nullptr);
    EXPECT_EQ(find_flash_device("does_not_exist"), nullptr);
    EXPECT_EQ(find_flash_device_index("M32170"), -1);
    EXPECT_EQ(find_flash_device_index("UNKNOWN_MCU"), -1);
}

TEST(FindFlashDevice, ExposesRomsizeForSizeValidation)
{
    const auto *device = find_flash_device("M32R_512KB");
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->romsize, 512u * 1024u);
}

TEST(FindFlashDevice, IndexAndPointerAgree)
{
    const int index = find_flash_device_index("M32R_384KB_1block");
    const flashdev_t *device = find_flash_device("M32R_384KB_1block");

    ASSERT_GE(index, 0);
    ASSERT_NE(device, nullptr);
    EXPECT_STREQ(flashdevices[index].name, "M32R_384KB_1block");
    EXPECT_STREQ(device->name, "M32R_384KB_1block");
    EXPECT_EQ(device->romsize, flashdevices[index].romsize);
    EXPECT_EQ(device->fblocks[0].start, flashdevices[index].fblocks[0].start);
}

TEST(FlashDeviceTable, MatchesExpectedSummariesAndNamedAnomalies)
{
    struct FlashDeviceSummary
    {
        const char *name;
        std::uint32_t romsize;
        unsigned numblocks;
        std::uint32_t firstBlockStart;
        std::uint32_t finalBlockEnd;
    };

    static constexpr FlashDeviceSummary kExpected[] = {
        {"M32R_128KB", 0x20000, 2, 0x0, 0x20000},
        {"M32R_256KB", 0x40000, 7, 0x0, 0x40000},
        {"M32R_384KB", 0x60000, 9, 0x0, 0x60000},
        {"M32R_512KB", 0x80000, 11, 0x0, 0x80000},
        {"M32R_512KB_1block", 0x80000, 1, 0x0, 0x80000},
        {"M32R_512KB_4blocks", 0x80000, 4, 0x0, 0x80000},
        {"M32R_384KB_1block", 0x60000, 1, 0x8000, 0x60000},
        {"MC68HC16Y5", 0x28000, 10, 0x0, 0x30000},
        {"MC68HC16Y5_TPU", 0x1000, 4, 0x60000, 0x64000},
        {"SH7051", 0x40000, 1, 0x0, 0x40000},
        {"SH7055", 0x80000, 16, 0x0, 0x80000},
        {"SH7058", 0x100000, 16, 0x0, 0x100000},
        {"SH7058_1block", 0x100000, 1, 0x0, 0x100000},
        {"SH7058d", 0x100000, 16, 0x0, 0x100000},
        {"SH7059d", 0x180000, 16, 0x0, 0x180000},
        {"SH72543d", 0x200000, 1, 0x8000, 0x1fff00},
        {"SH72531", 0x140000, 3, 0x0, 0x138000},
        {"N83M_4MB", 0x3e4000, 3, 0x08f9c000, 0x09380000},
        {"N83M_1_5MB", 0x174000, 3, 0x08f9c000, 0x09120000},
        {"SH72543R", 0x200000, 2, 0x0, 0x200000},
        {"MH8104", 0x80000, 4, 0x0, 0x80000},
        {"MH5006", 0x100000, 4, 0x0, 0x100000},
        {"MH8111", 0x180000, 4, 0x0, 0x180000},
        {"M3779x", 0x10000, 1, 0x8000, 0x17fff},
        {"M3775x", 0x10000, 1, 0x1000, 0x10fff},
    };
    constexpr std::size_t kCount = sizeof(kExpected) / sizeof(kExpected[0]);

    for (std::size_t deviceIndex = 0; deviceIndex < kCount; ++deviceIndex)
    {
        const flashdev_t& actual = flashdevices[deviceIndex];
        const FlashDeviceSummary& summary = kExpected[deviceIndex];
        EXPECT_STREQ(actual.name, summary.name);
        EXPECT_EQ(actual.romsize, summary.romsize);
        EXPECT_EQ(actual.numblocks, summary.numblocks);
        EXPECT_EQ(actual.fblocks[0].start, summary.firstBlockStart);
        const flashblock& finalBlock = actual.fblocks[actual.numblocks - 1];
        EXPECT_EQ(finalBlock.start + finalBlock.len, summary.finalBlockEnd);
        for (unsigned blockIndex = 0; blockIndex < actual.numblocks; ++blockIndex)
        {
            EXPECT_GT(actual.fblocks[blockIndex].len, 0u);
            if (blockIndex > 0)
            {
                EXPECT_LE(actual.fblocks[blockIndex - 1].start, actual.fblocks[blockIndex].start);
            }
        }
    }

    const flashdev_t& sentinel = flashdevices[kCount];
    EXPECT_EQ(sentinel.name, nullptr);
    EXPECT_EQ(sentinel.romsize, std::uint32_t(0));
    EXPECT_EQ(sentinel.numblocks, 0u);
    EXPECT_EQ(sentinel.fblocks, nullptr);
    EXPECT_EQ(sentinel.rblocks, nullptr);
    EXPECT_EQ(sentinel.kblocks, nullptr);
    EXPECT_EQ(sentinel.eblocks, nullptr);

    const flashdev_t *sh72531 = find_flash_device("SH72531");
    const flashdev_t *mc68 = find_flash_device("MC68HC16Y5");
    const flashdev_t *n83 = find_flash_device("N83M_1_5MB");
    const flashdev_t *tpu = find_flash_device("MC68HC16Y5_TPU");
    ASSERT_NE(sh72531, nullptr);
    ASSERT_NE(mc68, nullptr);
    ASSERT_NE(n83, nullptr);
    ASSERT_NE(tpu, nullptr);

    // These checks freeze the named quirks in the current table; they do
    // not declare the anomalous ranges correct.

    // SH72531 block 2 starts 0x8000 bytes before block 1 ends.
    EXPECT_EQ(sh72531->fblocks[1].start + sh72531->fblocks[1].len - sh72531->fblocks[2].start,
              std::uint32_t(0x8000));

    // MC68HC16Y5 leaves 0x8000 between block 7 and block 8 and its final two
    // blocks extend past the declared 0x28000 ROM size.
    EXPECT_EQ(mc68->fblocks[8].start - (mc68->fblocks[7].start + mc68->fblocks[7].len),
              std::uint32_t(0x8000));
    EXPECT_EQ(mc68->fblocks[9].start + mc68->fblocks[9].len, std::uint32_t(0x30000));

    // N83M_1_5MB block coverage extends 0x10000 past base + declared ROM size.
    EXPECT_EQ(n83->fblocks[2].start + n83->fblocks[2].len - (n83->fblocks[0].start + n83->romsize),
              std::uint32_t(0x10000));

    // MC68HC16Y5_TPU declares 0x1000 ROM bytes but exposes four contiguous
    // 0x1000 blocks; three blocks extend beyond base + romsize.
    EXPECT_EQ(tpu->fblocks[3].start + tpu->fblocks[3].len - (tpu->fblocks[0].start + tpu->romsize),
              std::uint32_t(0x3000));
}
```

- [ ] **Step 3: Add the target and run to verify it fails**

Add to `src/backend/flash/BUILD.bazel`:

```python
# Portable. No Qt, no platform, no filesystem, no thread. Moved here from
# //src/backend/checksum in step 5e: the flashdevices[] table is flash-device
# geometry consumed by 30 flash operations and one checksum dispatcher, so
# backend/checksum was the wrong owner.
cc_library(
    name = "flash_device_lookup",
    srcs = ["flash_device_lookup.cpp"],
    hdrs = ["flash_device_lookup.h"],
    deps = ["//src/backend/definitions:models"],
)

fastecu_portable_gtest(
    name = "flash_device_lookup_test",
    srcs = ["flash_device_lookup_test.cpp"],
    deps = [":flash_device_lookup"],
)
```

and add to that file's `load` statements:

```python
load("//bazel:gtest_targets.bzl", "fastecu_portable_gtest")
```

Run: `bazel test --config=release //src/backend/flash:flash_device_lookup_test`
Expected: FAIL — `src/backend/flash/flash_device_lookup.h` does not exist.

- [ ] **Step 4: Write the implementation**

Create `src/backend/flash/flash_device_lookup.h`:

```cpp
#pragma once

#include <string_view>

#include "src/backend/definitions/kernelmemorymodels.h"

namespace fastecu::flash
{

// Returns the flashdevices[] entry whose name matches mcu_type, or nullptr.
const flashdev_t *find_flash_device(std::string_view mcu_type);

// Returns the flashdevices[] index whose name matches mcu_type, or -1.
// Callers index the global table directly (flashdevices[i].fblocks[n].len
// and similar) throughout the legacy flash operations, so the index form is
// load-bearing and not merely a convenience over the pointer form.
int find_flash_device_index(std::string_view mcu_type);

} // namespace fastecu::flash
```

Create `src/backend/flash/flash_device_lookup.cpp`:

```cpp
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{

int find_flash_device_index(std::string_view mcu_type)
{
    for (int i = 0; flashdevices[i].name != nullptr; ++i)
    {
        if (mcu_type == flashdevices[i].name)
        {
            return i;
        }
    }
    return -1;
}

const flashdev_t *find_flash_device(std::string_view mcu_type)
{
    const int index = find_flash_device_index(mcu_type);
    if (index < 0)
    {
        return nullptr;
    }
    return &flashdevices[index];
}

} // namespace fastecu::flash
```

- [ ] **Step 5: Run to verify it passes**

Run: `bazel test --config=release //src/backend/flash:flash_device_lookup_test`
Expected: PASS, 5 cases green.

- [ ] **Step 6: Re-point `dispatch` and delete the old target**

In `src/backend/checksum/dispatch.{h,cpp}`, change the include from `src/backend/checksum/flash_device_lookup.h` to `src/backend/flash/flash_device_lookup.h`, and change every `fastecu::checksum::find_flash_device` / bare `find_flash_device` reference to `fastecu::flash::find_flash_device`. Locate them first:

```bash
grep -rn "find_flash_device\|flash_device_lookup" src/backend/checksum/
```

In `src/backend/checksum/BUILD.bazel`, delete the `flash_device_lookup` `cc_library` and its `fastecu_portable_gtest`, and change `dispatch`'s deps from `":flash_device_lookup"` to `"//src/backend/flash:flash_device_lookup"`.

Delete the files:

```bash
git rm src/backend/checksum/flash_device_lookup.h \
       src/backend/checksum/flash_device_lookup.cpp \
       src/backend/checksum/flash_device_lookup_test.cpp
```

- [ ] **Step 7: Update the portable-closure registration**

In `scripts/check-portable-closure.py`, remove `"flash_device_lookup"` from the `src/backend/checksum` set and add it to the `src/backend/flash` set:

```python
    ROOT / "src/backend/flash": {
        "flash_types",
        "flash_plan",
        "flash_validation",
        "flash_executor",
        "flash_device_lookup",
    },
    ...
    ROOT / "src/backend/checksum": {
        "checksum_selection",
        "dispatch",
    },
```

- [ ] **Step 8: Verify the closure check is non-vacuous for the new entry**

Temporarily add `"//src/algorithms/protocol:qt_compat"` to `flash_device_lookup`'s deps:

Run: `bazel test --config=release //:portable_closure`
Expected: FAIL, naming `//src/backend/flash:flash_device_lookup` as reaching Qt.

Then remove the temporary dep, and temporarily rename the target to `flash_device_lookup2`:

Run: `bazel test --config=release //:portable_closure`
Expected: FAIL, reporting the registered target `flash_device_lookup` is missing.

Restore the name. Both failure modes proven.

- [ ] **Step 9: Full gate**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Expected: all pass.

- [ ] **Step 10: Commit**

```bash
git add -A src/backend/flash src/backend/checksum scripts/check-portable-closure.py
git commit -m "refactor(flash): move portable flash-device lookup out of checksum (5e-1)

//src/backend/checksum:flash_device_lookup was a string_view clone of
FlashUtils::findFlashDeviceIndex, made to avoid touching the Qt callers.
Collapses both into one portable //src/backend/flash target that also
exposes the index form the 30 legacy flash call sites need. Test merges
the assertions from both predecessor suites.

Verified non-vacuous: //:portable_closure fails both when the target
reaches Qt and when it is absent."
```

---

## Task 4: Create the platform-side `legacy_flash_utils`

Moves the Qt/serial half of `flash_utils` to the package that already owns its callers. The `FlashUtils::` namespace name is kept so the 30 call sites change only their `#include`.

**Files:**
- Create: `src/platform/desktop/common/flash/legacy/legacy_flash_utils.{h,cpp}`
- Create: `src/platform/desktop/common/flash/legacy/legacy_flash_utils_test.cpp`
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::flash::find_flash_device_index(std::string_view)` from Task 3.
- Produces, in namespace `FlashUtils`, declared in `src/platform/desktop/common/flash/legacy/legacy_flash_utils.h`:
  - `int findFlashDeviceIndex(const QString& mcuType)`
  - `void configureIso15765Can(SerialPortActions *serial, const QString& canSpeed, quint32 sourceAddress, quint32 destinationAddress, bool use29BitId = false)`

  Task 5 re-points all 30 call sites at this header.

The package's `srcs` and `normal_hdrs` are globs over `*.cpp` / `*.h`, so the new files join `legacy_flash_operations` automatically. No `MOC_HDRS` entry — `legacy_flash_utils.h` declares no `Q_OBJECT`.

- [ ] **Step 1: Write the failing test**

Create `src/platform/desktop/common/flash/legacy/legacy_flash_utils_test.cpp`. These are the two `configureIso15765Can_*` cases from `src/backend/flash/flash_utils_test.cpp:146-178`, plus one new case pinning the forwarding shim:

```cpp
#include <QtTest>

#include "src/platform/desktop/common/flash/legacy/legacy_flash_utils.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

class TestLegacyFlashUtils : public QObject
{
    Q_OBJECT
  private slots:
    void configureIso15765Can_setsSharedCanTransportState()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new FakeBackend(); return fake; });

        FlashUtils::configureIso15765Can(&serial, "250000", 0x7E1, 0x7E9, true);

        QCOMPARE(serial.get_is_iso14230_connection(), false);
        QCOMPARE(serial.get_add_iso14230_header(), false);
        QCOMPARE(serial.get_is_can_connection(), false);
        QCOMPARE(serial.get_is_iso15765_connection(), true);
        QCOMPARE(serial.get_is_29_bit_id(), true);
        QCOMPARE(serial.get_can_speed(), QString("250000"));
        QCOMPARE(serial.get_iso15765_source_address(), quint32(0x7E1));
        QCOMPARE(serial.get_iso15765_destination_address(), quint32(0x7E9));
    }

    void configureIso15765Can_defaultsTo11BitCanIds()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new FakeBackend(); return fake; });

        FlashUtils::configureIso15765Can(&serial, "500000", 0x7E0, 0x7E8);

        QCOMPARE(serial.get_is_29_bit_id(), false);
        QCOMPARE(serial.get_can_speed(), QString("500000"));
        QCOMPARE(serial.get_iso15765_source_address(), quint32(0x7E0));
        QCOMPARE(serial.get_iso15765_destination_address(), quint32(0x7E8));
    }

    void findFlashDeviceIndex_forwardsToPortableLookup()
    {
        // The shim exists only to keep 30 legacy call sites on QString; it
        // must agree with the portable lookup exactly, including the
        // not-found sentinel.
        QCOMPARE(FlashUtils::findFlashDeviceIndex("M32R_384KB_1block"),
                 fastecu::flash::find_flash_device_index("M32R_384KB_1block"));
        QCOMPARE(FlashUtils::findFlashDeviceIndex("UNKNOWN_MCU"), -1);
        QVERIFY(FlashUtils::findFlashDeviceIndex("M32R_512KB") >= 0);
    }
};

QTEST_GUILESS_MAIN(TestLegacyFlashUtils)

#include "legacy_flash_utils_test.moc"
```

- [ ] **Step 2: Add the test target and run to verify it fails**

Add to `src/platform/desktop/common/flash/legacy/BUILD.bazel`:

```python
fastecu_qttest(
    name = "test_legacy_flash_utils",
    src = "legacy_flash_utils_test.cpp",
    deps = [
        ":legacy_flash_operations",
        "//src/platform/desktop/common/serial/testing:fake_serial_backend",
    ],
)
```

Run: `bazel test --config=release //src/platform/desktop/common/flash/legacy:test_legacy_flash_utils`
Expected: FAIL — `legacy_flash_utils.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `src/platform/desktop/common/flash/legacy/legacy_flash_utils.h`:

```cpp
#pragma once

#include <QString>

#include "src/backend/flash/flash_device_lookup.h"

class SerialPortActions;

// TRANSITIONAL. Relocated from //src/backend/flash in step 5e so that
// backend no longer depends on src/platform. The FlashUtils namespace name
// is retained so the 30 legacy call sites changed only their #include.
//
// findFlashDeviceIndex is a QString-typed shim over the portable
// fastecu::flash::find_flash_device_index; each flash family that migrates
// in the step-5 tail should convert its own call sites to the portable form,
// and this shim is deleted when the last one does.
namespace FlashUtils
{
int findFlashDeviceIndex(const QString& mcuType);

void configureIso15765Can(SerialPortActions *serial,
                          const QString& canSpeed,
                          quint32 sourceAddress,
                          quint32 destinationAddress,
                          bool use29BitId = false);
} // namespace FlashUtils
```

Create `src/platform/desktop/common/flash/legacy/legacy_flash_utils.cpp` (`configureIso15765Can`'s body copied verbatim from `src/backend/flash/flash_utils.cpp:54-68`):

```cpp
#include "src/platform/desktop/common/flash/legacy/legacy_flash_utils.h"

#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace FlashUtils
{

int findFlashDeviceIndex(const QString& mcuType)
{
    return fastecu::flash::find_flash_device_index(mcuType.toStdString());
}

void configureIso15765Can(SerialPortActions *serial,
                          const QString& canSpeed,
                          quint32 sourceAddress,
                          quint32 destinationAddress,
                          bool use29BitId)
{
    serial->set_is_iso14230_connection(false);
    serial->set_add_iso14230_header(false);
    serial->set_is_can_connection(false);
    serial->set_is_iso15765_connection(true);
    serial->set_is_29_bit_id(use29BitId);
    serial->set_can_speed(canSpeed);
    serial->set_iso15765_source_address(sourceAddress);
    serial->set_iso15765_destination_address(destinationAddress);
}

} // namespace FlashUtils
```

- [ ] **Step 4: Add the portable dep**

In `src/platform/desktop/common/flash/legacy/BUILD.bazel`, add to `legacy_flash_operations`'s `deps`:

```python
        "//src/backend/flash:flash_device_lookup",
```

Do **not** remove `"//src/backend/flash"` yet — the 30 call sites still include the old header. Task 5 removes it.

- [ ] **Step 5: Run to verify it passes**

Run: `bazel test --config=release //src/platform/desktop/common/flash/legacy:test_legacy_flash_utils`
Expected: PASS, 3 cases green.

Note there are now two `FlashUtils::findFlashDeviceIndex` and two `FlashUtils::configureIso15765Can` declarations reachable in this package — one from each header. They have identical signatures and identical behavior, so any translation unit including only one resolves cleanly; Task 5 deletes the old pair before any TU includes both.

- [ ] **Step 6: Full gate**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/platform/desktop/common/flash/legacy/
git commit -m "feat(flash): add platform-side legacy_flash_utils (5e-1)

configureIso15765Can moved verbatim from backend, plus a QString shim over
the portable flash-device lookup. Keeps the FlashUtils namespace so the 30
legacy call sites change only their include. Old backend header still
present; the next commit re-points the call sites and deletes it."
```

---

## Task 5: Re-point the 30 call sites and delete `//src/backend/flash:flash`

The include-only diff. After this, `//src/backend/flash` is fully portable.

**Files:**
- Modify: 27 `.cpp` files under `src/platform/desktop/common/flash/legacy/{bdm,bootmode,ecu,jtag,tcu}/` (include line only)
- Modify: `src/platform/desktop/common/flash/legacy/flash_ecu_mitsu_m32r_can_operation.cpp:59` (inline `buildIso15765Request`)
- Modify: `src/platform/desktop/common/flash/legacy/flash_ecu_mitsu_m32r_can_operation_test.cpp` (absorb the deleted assertion)
- Modify: `src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs.cpp:39`
- Delete: `src/backend/flash/flash_utils.{h,cpp}`, `src/backend/flash/flash_utils_test.cpp`
- Modify: `src/backend/flash/BUILD.bazel`, `src/ui/desktop/BUILD.bazel`, `src/ui/desktop/flash/ecu/BUILD.bazel`, `src/platform/desktop/common/flash/legacy/BUILD.bazel`
- Modify: `scripts/check-serial-compat-allowlist.py`, `src/platform/desktop/common/serial/BUILD.bazel`

**Interfaces:**
- Consumes: `FlashUtils::findFlashDeviceIndex` / `FlashUtils::configureIso15765Can` from Task 4; `fastecu::checksum::cks_add8` from Task 2.
- Produces: `//src/backend/flash` with no `QT_DEPS` and no `serial_qt_compat` dep. No new symbols.

- [ ] **Step 1: Enumerate the call sites exactly**

```bash
grep -rln 'src/backend/flash/flash_utils.h' src/ | sort
```

Expected: 27 legacy operation `.cpp` files, plus `src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs.cpp`, plus `src/backend/flash/flash_utils_test.cpp`, plus `src/backend/flash/flash_utils.cpp`. Record the count — it is the completion check for Step 2.

- [ ] **Step 2: Rewrite the include in every legacy caller**

```bash
grep -rl 'src/backend/flash/flash_utils.h' \
    src/platform/desktop/common/flash/legacy/ src/ui/desktop/flash/ecu/ \
  | xargs sed -i '' \
      's|src/backend/flash/flash_utils.h|src/platform/desktop/common/flash/legacy/legacy_flash_utils.h|'
```

(On Linux use `sed -i` without the `''`.) Then confirm nothing outside `src/backend/flash` still references the old path:

```bash
grep -rn 'src/backend/flash/flash_utils.h' src/ | grep -v '^src/backend/flash/'
```

Expected: no output.

- [ ] **Step 3: Inline `buildIso15765Request` at its single call site**

In `src/platform/desktop/common/flash/legacy/flash_ecu_mitsu_m32r_can_operation.cpp:59`, replace:

```cpp
    return FlashUtils::buildIso15765Request(0x7E0, sidPayload);
```

with:

```cpp
    QByteArray request;
    bytes::appendU32Be(request, 0x7E0);
    request.append(sidPayload);
    return request;
```

Ensure that file includes `src/algorithms/protocol/qt_bytes.h` (it needs `bytes::appendU32Be`); add it if absent.

- [ ] **Step 4: Move the deleted function's assertion into the operation's own test**

`buildIso15765Request_prependsBigEndianSourceAddress` is being deleted with its function, but the behavior it pinned — big-endian source-address prefixing — now lives inline in the operation. Add to `src/platform/desktop/common/flash/legacy/flash_ecu_mitsu_m32r_can_operation_test.cpp`, inside its existing test class's `private slots:` section:

```cpp
    void iso15765Request_prependsBigEndianSourceAddress()
    {
        // Formerly FlashUtils::buildIso15765Request, inlined in step 5e at
        // its single call site. The 4-byte big-endian address prefix is the
        // wire format, so it is pinned here rather than lost with the helper.
        QByteArray request;
        bytes::appendU32Be(request, 0x7E0);
        request.append(QByteArray::fromHex("1081"));
        QCOMPARE(request, QByteArray::fromHex("000007e01081"));

        QByteArray extended;
        bytes::appendU32Be(extended, 0x18DA10F1);
        extended.append(QByteArray::fromHex("1081"));
        QCOMPARE(extended, QByteArray::fromHex("18da10f11081"));
    }
```

Ensure that test file includes `src/algorithms/protocol/qt_bytes.h`.

- [ ] **Step 5: Run the legacy package's tests**

```bash
bazel test --config=release //src/platform/desktop/common/flash/legacy:all
```

Expected: PASS. The 30 call sites now resolve through `legacy_flash_utils.h`.

- [ ] **Step 6: Delete the old files and target**

```bash
git rm src/backend/flash/flash_utils.h src/backend/flash/flash_utils.cpp \
       src/backend/flash/flash_utils_test.cpp
```

In `src/backend/flash/BUILD.bazel`, delete the `qt_cc_library(name = "flash", ...)` block and the `fastecu_qttest(name = "test_flash_utils", ...)` block, and drop now-unused symbols from the `load` line (`COMMON_COPTS`, `QT_DEPS`, `fastecu_qttest`, `qt_cc_library` — keep whatever other targets in the file still use).

- [ ] **Step 7: Re-point the three remaining `//src/backend/flash` dependents**

Verified against the build graph, not the `flash` target's comment — which is stale on two of its four named consumers (`//src/backend/definitions` has no such dep at all, and `test_flash_utils` is co-located here, not under `tests/`).

- `src/ui/desktop/BUILD.bazel:81` — remove `"//src/backend/flash"`. `ecu_operations.cpp` already got `//src/algorithms/checksum` in Task 2.
- `src/ui/desktop/flash/ecu/BUILD.bazel:19` — replace `"//src/backend/flash"` with `"//src/platform/desktop/common/flash/legacy"`. (`flash_ecu_subaru_unisia_jecs.cpp` calls `FlashUtils::findFlashDeviceIndex`.)
- `src/platform/desktop/common/flash/legacy/BUILD.bazel:68` — remove `"//src/backend/flash"`; `":flash_device_lookup"`'s label was added in Task 4.

Then confirm nothing depends on the deleted label:

```bash
bazel query 'rdeps(//..., //src/backend/flash:flash)' 2>&1 | head
```

Expected: an error that the target does not exist.

- [ ] **Step 8: Swap the allowlist entry**

The legacy flash package reached `serial_port_actions.h` transitively through `//src/backend/flash:flash`. With that target gone it needs its own edge.

In `src/platform/desktop/common/serial/BUILD.bazel`, in `serial_qt_compat`'s `visibility` list, delete `"//src/backend/flash:__pkg__"` and add, in sorted position among the TRANSITIONAL entries:

```python
        # TRANSITIONAL, step 5e. Replaces the //src/backend/flash entry, which
        # existed only because the 27 legacy operation classes reached
        # serial_port_actions.h *through* backend/flash's flash_utils target --
        # a backend -> platform layer inversion used as a visibility channel.
        # This is the same debt, correctly attributed: a platform -> platform
        # sibling edge, which the layering rules do not prohibit. Drained by
        # the step-5 tail as each flash family migrates off the full facade.
        "//src/platform/desktop/common/flash/legacy:__pkg__",
```

In `scripts/check-serial-compat-allowlist.py`, make the same swap in `FROZEN`: delete the `"//src/backend/flash:__pkg__"` entry together with its multi-line comment block, and add `"//src/platform/desktop/common/flash/legacy:__pkg__"` with a one-line comment pointing at the design doc.

Add `"//src/platform/desktop/common/flash/legacy"` to `legacy_flash_operations`'s deps in that package's BUILD? No — the package depends on `serial_qt_compat` directly now:

```python
        "//src/platform/desktop/common/serial:serial_qt_compat",
```

Add that to `legacy_flash_operations`'s `deps`, and delete the stale comment block above the removed `"//src/backend/flash"` entry that explains why the package deliberately avoided this dep.

- [ ] **Step 9: Verify the allowlist swap**

Run: `bazel test --config=release //:serial_compat_allowlist`
Expected: PASS with 13 entries.

Then verify non-vacuity: temporarily add `"//src/backend/checksum:__pkg__"` to the BUILD `visibility` list only.

Run: `bazel test --config=release //:serial_compat_allowlist`
Expected: FAIL, printing `//src/backend/checksum:__pkg__` as a new entry. Remove it and re-run; expected PASS.

- [ ] **Step 10: Confirm backend no longer reaches platform**

```bash
bazel query 'somepath(//src/backend/..., //src/platform/...)' 2>&1 | head
```

Expected: no path from any `src/backend` target into `src/platform`.

- [ ] **Step 11: Full gate**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Expected: all pass.

- [ ] **Step 12: Commit**

```bash
git add -A
git commit -m "refactor(flash): delete backend/flash Qt target, swap allowlist entry (5e-1)

Re-points 30 legacy call sites at the platform-side legacy_flash_utils
(include-only change), inlines the single-caller buildIso15765Request, and
deletes flash_utils entirely. //src/backend/flash is now fully portable.

The serial_qt_compat allowlist swaps //src/backend/flash for
//src/platform/desktop/common/flash/legacy: the same debt, correctly
attributed. Backend no longer depends on src/platform at all.

Allowlist: 13 entries, 0 in backend."
```

---

## Task 6: Characterize the checksum flow before moving it

Before deleting `FileActions::checksum_correction`, capture what it does — including its log output, which the design flags as the thing most likely to be silently lost.

**Files:**
- Modify: `src/backend/definitions/file_actions_parsing_test.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: a passing characterization of the four `LOG_D` lines and one `LOG_E` line, used as the fidelity reference by Task 8.

- [ ] **Step 1: Write the characterization test**

The suite already has `spyContainsMessage` and an `errorSpy`/`debugSpy` convention (see the tests around `file_actions_parsing_test.cpp:1275`). Add this alongside the two existing `checksum_correction_*` cases:

```cpp
    void checksum_correction_emits_protocol_make_checksum_and_size_debug_lines()
    {
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        actions.ConfigValuesStruct.flash_protocol_selected_make = "Subaru";
        actions.ConfigValuesStruct.flash_protocol_selected_checksum = "yes";
        actions.ConfigValuesStruct.flash_protocol_selected_protocol_name = "sub_ecu_hitachi_m32r_can";
        actions.ConfigValuesStruct.flash_protocol_selected_mcu = "M32170";

        QSignalSpy debugSpy(&actions, &FileActions::LOG_D);
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);

        FileActions::EcuCalDefStructure ecu;
        ecu.McuType = "M32170";
        ecu.RomId = "39670016";
        ecu.FullRomData = QByteArray(100, '\0');

        actions.checksum_correction(&ecu);

        // These five lines are the observable output of the unknown-MCU path
        // and must survive the move to ChecksumCorrectionCommand verbatim.
        QVERIFY(spyContainsMessage(debugSpy, "Protocol: sub_ecu_hitachi_m32r_can"));
        QVERIFY(spyContainsMessage(debugSpy, "Make: Subaru"));
        QVERIFY(spyContainsMessage(debugSpy, "Checksum: yes"));
        QVERIFY(spyContainsMessage(errorSpy, "Unknown MCU type: M32170"));
    }
```

- [ ] **Step 2: Run it**

Run: `bazel test --config=release //src/backend/definitions:all`
Expected: PASS. If any `QVERIFY` fails, the emitted text differs from what this plan recorded — **fix the assertion to match the real output**, not the other way round. This test's job is to record reality.

- [ ] **Step 3: Record the exact strings**

Copy the four asserted substrings into a scratch note; Task 8 reproduces them in the UI command. The two remaining `LOG_D` lines (`ecuCalDef->McuType: ...` and `Size: 0x... -> 0x...`) are only reachable on the *known*-MCU path — read them directly from `src/backend/definitions/file_actions.cpp:1693-1696` and record them too.

- [ ] **Step 4: Commit**

```bash
git add src/backend/definitions/file_actions_parsing_test.cpp
git commit -m "test(definitions): characterize checksum_correction log output (5e-2)

Pins the four debug/error lines checksum_correction emits before the method
moves to src/ui/desktop/checksum. The design names silently-dropped log
lines as this slice's main fidelity risk."
```

---

## Task 7: Create the `ChecksumCorrectionCommand` package

A new package with **explicit `srcs`** — `src/ui/desktop`'s own `glob(["*.cpp"])` has no `*_test.cpp` exclusion, so a test co-located there would link into the production library.

**Files:**
- Create: `src/ui/desktop/checksum/BUILD.bazel`
- Create: `src/ui/desktop/checksum/checksum_correction_command.{h,cpp}`
- Create: `src/ui/desktop/checksum/checksum_correction_command_test.cpp`

**Interfaces:**
- Consumes: `fastecu::checksum::apply_checksum_correction` and `ChecksumSelection` from `//src/backend/checksum:dispatch`; `fastecu::flash::find_flash_device` from Task 3.
- Produces, in namespace `fastecu::ui`, declared in `src/ui/desktop/checksum/checksum_correction_command.h`:
  - `struct ChecksumCorrectionResult { std::optional<bytes::Bytes> corrected_rom_data; bool canceled_due_to_missing_module = false; bool unknown_mcu_type = false; }`
  - `class ChecksumCorrectionCommand` with `ChecksumCorrectionResult run(bytes::ByteView rom_data, bool use_romraider_definition, bool use_ecuflash_definition, const fastecu::checksum::ChecksumSelection& selection, QWidget *parent)` and four `protected virtual` seams: `confirmProceedWithoutDefinition(QWidget*)`, `showBadRomSizeDialog(QWidget*)`, `confirmProceedWithoutChecksumModule()`, `showFamilyResultDialog(const ChecksumResult&)`.

**Namespace note, verified against the tree:** `ChecksumResult`
(`src/algorithms/checksum/checksum_result.h`) is in the **global** namespace —
the deleted adapter referred to it unqualified only because that code sat
inside `namespace fastecu::checksum`. From `fastecu::ui` it must stay
unqualified, **not** written as `fastecu::checksum::ChecksumResult`.
`ChecksumSelection` and `ChecksumCorrectionOutcome`
(`src/backend/checksum/checksum_selection.h`) *are* in `fastecu::checksum`.

  Task 8 constructs this from `MainWindow`.

- [ ] **Step 1: Write the failing test**

Create `src/ui/desktop/checksum/checksum_correction_command_test.cpp`. This is `src/backend/checksum/legacy_checksum_adapter_test.cpp` with `LegacyChecksumAdapter` → `ChecksumCorrectionCommand`, `checksum_correction(...)` → `run(...)`, and `LegacyChecksumAdapterResult` → `ChecksumCorrectionResult`. Read the original in full and port **every** case; the header below shows the shape:

```cpp
#include "src/ui/desktop/checksum/checksum_correction_command.h"

#include <QApplication>
#include <QTimer>
#include <QWidget>
#include <gtest/gtest.h>

// ChecksumResult is in the global namespace -- see the namespace note above.
using fastecu::checksum::ChecksumSelection;
using fastecu::ui::ChecksumCorrectionCommand;
using fastecu::ui::ChecksumCorrectionResult;

namespace
{

class TestableChecksumCommand : public ChecksumCorrectionCommand
{
  public:
    bool proceedWithoutDefinitionAnswer = true; // "DO IT!" by default
    bool cancelWithoutModuleAnswer = false;     // "OK" (proceed) by default
    int badRomSizeDialogCount = 0;
    int familyResultDialogCount = 0;
    ChecksumResult lastFamilyResult;

  protected:
    bool confirmProceedWithoutDefinition(QWidget *) override
    {
        return proceedWithoutDefinitionAnswer;
    }
    void showBadRomSizeDialog(QWidget *) override
    {
        ++badRomSizeDialogCount;
    }
    bool confirmProceedWithoutChecksumModule() override
    {
        return cancelWithoutModuleAnswer;
    }
    void showFamilyResultDialog(const ChecksumResult& family_result) override
    {
        ++familyResultDialogCount;
        lastFamilyResult = family_result;
    }
};

ChecksumSelection subaruM32rKlineSelection()
{
    ChecksumSelection selection;
    selection.make = "Subaru";
    selection.checksum_flag = "yes";
    selection.flash_method = "sub_ecu_hitachi_m32r_kline";
    selection.mcu_type = "M32R_512KB";
    selection.rom_id = "39670016";
    return selection;
}

} // namespace
```

Then add the two cases recovered from `file_actions_parsing_test.cpp:1283-1332`, which the deleted `FileActions` method carried:

```cpp
TEST(ChecksumCorrectionCommand, UnknownMcuTypeReturnsUnmodifiedRomAndRunsNoDialog)
{
    // "M32170" is sub_ecu_mitsu_m32r_can's real, currently shipped <mcu>
    // value in resources/shared/config/protocols.cfg; it has no
    // flashdevices[] entry. Formerly checksum_correction's early return.
    TestableChecksumCommand command;
    ChecksumSelection selection = subaruM32rKlineSelection();
    selection.mcu_type = "M32170";

    const bytes::Bytes rom(100, bytes::Byte{0});
    const ChecksumCorrectionResult result =
        command.run(bytes::ByteView(rom), true, false, selection, nullptr);

    EXPECT_TRUE(result.unknown_mcu_type);
    EXPECT_FALSE(result.corrected_rom_data.has_value());
    EXPECT_EQ(command.badRomSizeDialogCount, 0);
    EXPECT_EQ(command.familyResultDialogCount, 0);
}

TEST(ChecksumCorrectionCommand, ValidMcuCorrectsRomAndReturnsChangedBytes)
{
    TestableChecksumCommand command;
    ChecksumSelection selection;
    selection.make = "Subaru";
    selection.checksum_flag = "yes";
    selection.flash_method = "sub_ecu_denso_sh7055";
    selection.mcu_type = "SH7055";
    selection.rom_id = "39670016";

    const bytes::Bytes rom(524288, bytes::Byte{0}); // SH7055 romsize -> Corrected
    const ChecksumCorrectionResult result =
        command.run(bytes::ByteView(rom), true, false, selection, nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
    EXPECT_EQ(result.corrected_rom_data->size(), 524288u);
    EXPECT_NE(*result.corrected_rom_data, rom);
    EXPECT_EQ(command.familyResultDialogCount, 1);
}
```

Note these two no longer need the `QTimer::singleShot` modal auto-dismiss the `FileActions` versions used — the seams are overridden, so no real `QMessageBox` is ever shown. That is a direct benefit of the move.

- [ ] **Step 2: Create the BUILD file and run to verify it fails**

Create `src/ui/desktop/checksum/BUILD.bazel`:

```python
load("//bazel:gtest_targets.bzl", "fastecu_gtest")
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "qt_cc_library")

package(default_visibility = ["//src/ui:__subpackages__"])

# Qt-linked (QMessageBox/QString) but no QObject/Q_OBJECT, so the header goes
# through normal_hdrs rather than hdrs. Explicit srcs -- this package must not
# adopt src/ui/desktop's glob(["*.cpp"]), which has no *_test.cpp exclusion.
qt_cc_library(
    name = "checksum_correction_command",
    srcs = ["checksum_correction_command.cpp"],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["checksum_correction_command.h"],
    deps = QT_DEPS + [
        "//src/algorithms/protocol",
        "//src/backend/checksum:checksum_selection",
        "//src/backend/checksum:dispatch",
        "//src/backend/flash:flash_device_lookup",
    ],
)

fastecu_gtest(
    name = "checksum_correction_command_test",
    srcs = ["checksum_correction_command_test.cpp"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    deps = [":checksum_correction_command"],
)
```

Run: `bazel test --config=release //src/ui/desktop/checksum:checksum_correction_command_test`
Expected: FAIL — `checksum_correction_command.h` does not exist.

- [ ] **Step 3: Write the header**

Create `src/ui/desktop/checksum/checksum_correction_command.h`:

```cpp
#pragma once

#include <optional>

#include "src/algorithms/checksum/checksum_result.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/checksum/checksum_selection.h"

class QWidget;

namespace fastecu::ui
{

struct ChecksumCorrectionResult
{
    std::optional<bytes::Bytes> corrected_rom_data;
    bool canceled_due_to_missing_module = false;
    bool unknown_mcu_type = false;
};

// Owns the checksum-correction dialog sequence, hoisted out of
// FileActions::checksum_correction and the backend LegacyChecksumAdapter in
// step 5e so that no QMessageBox is raised from src/backend.
//
// The protected virtual seams let a test subclass script answers without
// showing a real modal QMessageBox -- same pattern the deleted adapter used.
class ChecksumCorrectionCommand
{
  public:
    virtual ~ChecksumCorrectionCommand() = default;

    ChecksumCorrectionResult run(bytes::ByteView rom_data, bool use_romraider_definition,
                                 bool use_ecuflash_definition,
                                 const fastecu::checksum::ChecksumSelection& selection,
                                 QWidget *parent);

  protected:
    // Returns true if the user chose to proceed anyway ("DO IT!"), false for
    // the default "OK" (abort).
    virtual bool confirmProceedWithoutDefinition(QWidget *parent);

    virtual void showBadRomSizeDialog(QWidget *parent);

    // Returns true if the user chose Cancel (abort correction), false for OK.
    virtual bool confirmProceedWithoutChecksumModule();

    virtual void showFamilyResultDialog(const ChecksumResult& family_result);
};

} // namespace fastecu::ui
```

`ChecksumResult` is unqualified here deliberately — it lives in the global
namespace, and `checksum_result.h` is already included above.

- [ ] **Step 4: Write the implementation**

Create `src/ui/desktop/checksum/checksum_correction_command.cpp`. The four dialog bodies are copied **verbatim** from `src/backend/checksum/legacy_checksum_adapter.cpp:14-69` — the exact strings, icons, titles and button roles matter. `run` is that file's `checksum_correction` plus the `find_flash_device` precheck lifted from `file_actions.cpp:1687-1692`:

```cpp
#include "src/ui/desktop/checksum/checksum_correction_command.h"

#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include "src/backend/checksum/dispatch.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::ui
{

using fastecu::checksum::ChecksumCorrectionOutcome;
// ChecksumResult needs no using-declaration: it is in the global namespace.

bool ChecksumCorrectionCommand::confirmProceedWithoutDefinition(QWidget *parent)
{
    QMessageBox msgBox(parent);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle("Calibration file");
    msgBox.setText("WARNING! No definition file linked to selected ROM, checksums are not calculated!\n\n"
                   "If you are sure that right protocol is selected and want to correct checksums anyway, press 'DO IT!' -button");
    QPushButton *okButton = msgBox.addButton(QMessageBox::Ok);
    msgBox.addButton(QObject::tr("DO IT!"), QMessageBox::NoRole);
    msgBox.exec();
    return msgBox.clickedButton() != okButton;
}

void ChecksumCorrectionCommand::showBadRomSizeDialog(QWidget *parent)
{
    QMessageBox::information(parent, QObject::tr("Checksum module"),
                             "Bad ROM size! Make sure that you have selected correct flash method!");
}

bool ChecksumCorrectionCommand::confirmProceedWithoutChecksumModule()
{
    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle("File - Checksum Warning");
    msgBox.setText("WARNING! There is no checksum module for this ROM!"
                   "                            Be aware that if this ROM need checksum correction it must be done with another software!");
    QPushButton *cancelButton = msgBox.addButton(QMessageBox::Cancel);
    msgBox.addButton(QMessageBox::Ok);
    msgBox.exec();
    return msgBox.clickedButton() == cancelButton;
}

void ChecksumCorrectionCommand::showFamilyResultDialog(const ChecksumResult& family_result)
{
    const QString message = QString::fromStdString(family_result.message);
    if (family_result.changed())
    {
        QMessageBox::information(nullptr, QObject::tr("Checksum Correction"),
                                 QObject::tr("Checksums corrected:\n\n%1").arg(message));
        return;
    }
    switch (family_result.status)
    {
    case ChecksumResult::Status::Disabled:
        QMessageBox::information(nullptr, QObject::tr("32-bit checksum"), message);
        break;
    case ChecksumResult::Status::InvalidSize:
    case ChecksumResult::Status::UnsupportedRom:
    case ChecksumResult::Status::ParseError:
        QMessageBox::warning(nullptr, QObject::tr("Checksum module"), message);
        break;
    case ChecksumResult::Status::Corrected:
    case ChecksumResult::Status::Unchanged:
        break;
    }
}

ChecksumCorrectionResult ChecksumCorrectionCommand::run(
    bytes::ByteView rom_data, bool use_romraider_definition, bool use_ecuflash_definition,
    const fastecu::checksum::ChecksumSelection& selection, QWidget *parent)
{
    ChecksumCorrectionResult result;

    // Formerly FileActions::checksum_correction's own precheck, ahead of the
    // adapter call: an unregistered MCU returns the ROM untouched and shows
    // no dialog at all.
    if (fastecu::flash::find_flash_device(selection.mcu_type) == nullptr)
    {
        result.unknown_mcu_type = true;
        return result;
    }

    if (!use_romraider_definition && !use_ecuflash_definition)
    {
        if (!confirmProceedWithoutDefinition(parent))
        {
            return result;
        }
    }

    const ChecksumCorrectionOutcome outcome =
        fastecu::checksum::apply_checksum_correction(rom_data, selection);
    switch (outcome.status)
    {
    case ChecksumCorrectionOutcome::Status::UnknownMcuType:
        // Unreachable: the find_flash_device precheck above returns first.
        // Handled defensively as a no-op, matching legacy's silent return.
        break;
    case ChecksumCorrectionOutcome::Status::BadRomSize:
        showBadRomSizeDialog(parent);
        break;
    case ChecksumCorrectionOutcome::Status::NoModuleForProtocol:
        if (selection.checksum_flag != "no")
        {
            result.canceled_due_to_missing_module = confirmProceedWithoutChecksumModule();
        }
        break;
    case ChecksumCorrectionOutcome::Status::FamilyRan:
        if (outcome.family_result.has_value())
        {
            if (outcome.family_result->ok())
            {
                result.corrected_rom_data = outcome.family_result->romData;
            }
            showFamilyResultDialog(*outcome.family_result);
        }
        break;
    }
    return result;
}

} // namespace fastecu::ui
```

- [ ] **Step 5: Run to verify it passes**

Run: `bazel test --config=release //src/ui/desktop/checksum:checksum_correction_command_test`
Expected: PASS — every ported case from the adapter suite plus the two recovered cases.

- [ ] **Step 6: Full gate**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Expected: all pass. `LegacyChecksumAdapter` still exists and is still wired into `FileActions`; both paths coexist until Task 8.

- [ ] **Step 7: Commit**

```bash
git add src/ui/desktop/checksum/
git commit -m "feat(ui): add ChecksumCorrectionCommand (5e-2)

Hoists the checksum dialog sequence out of backend's LegacyChecksumAdapter,
absorbing FileActions::checksum_correction's find_flash_device precheck.
Own package with explicit srcs -- src/ui/desktop's glob has no *_test.cpp
exclusion. The backend adapter is deleted in the next commit."
```

---

## Task 8: Delete the backend adapter and wire `MainWindow` to the command

**Files:**
- Delete: `src/backend/checksum/legacy_checksum_adapter.{h,cpp}`, `legacy_checksum_adapter_test.cpp`
- Modify: `src/backend/checksum/BUILD.bazel`
- Modify: `src/backend/definitions/file_actions.{h,cpp}`, `file_actions_parsing_test.cpp`
- Modify: `src/ui/desktop/mainwindow.{h,cpp}`, `menu_actions.cpp`, `BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::ui::ChecksumCorrectionCommand` from Task 7.
- Produces: `//src/backend/checksum` with no `QT_DEPS`. No new symbols.

- [ ] **Step 1: Re-verify which call sites are live**

`grep` alone is wrong here: four of the seven textual matches sit inside `/* */` blocks. Use comment-stripped parsing:

```bash
python3 - <<'EOF'
import re
src = open('src/ui/desktop/mainwindow.cpp').read()
stripped = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), src, flags=re.S)
print("live:", [i+1 for i, l in enumerate(stripped.split('\n')) if 'checksum_correction' in l])
EOF
```

Expected: `live: [1144, 1645, 1700]`. If the numbers differ, the file has shifted — use the printed set, not this plan's.

- [ ] **Step 2: Add the command to `MainWindow`**

In `src/ui/desktop/mainwindow.h`, beside the existing port members at lines 205-208:

```cpp
    fastecu::ui::ChecksumCorrectionCommand m_checksumCorrectionCommand;
```

and include `src/ui/desktop/checksum/checksum_correction_command.h`.

In `src/ui/desktop/BUILD.bazel`, add to the deps of the target compiling `mainwindow.cpp`:

```python
        "//src/ui/desktop/checksum:checksum_correction_command",
```

- [ ] **Step 3: Add the private helper that replaces the deleted method**

In `src/ui/desktop/mainwindow.cpp`, add a private helper reproducing what `FileActions::checksum_correction` did — selection construction, the four log lines recorded in Task 6, the command call, and the byte write-back:

```cpp
void MainWindow::runChecksumCorrection(FileActions::EcuCalDefStructure *ecuCalDef)
{
    const fastecu::checksum::ChecksumSelection selection{
        .make = configValues->flash_protocol_selected_make.toStdString(),
        .checksum_flag = configValues->flash_protocol_selected_checksum.toStdString(),
        .flash_method = configValues->flash_protocol_selected_protocol_name.toStdString(),
        .mcu_type = ecuCalDef->McuType.toStdString(),
        .rom_id = ecuCalDef->RomId.toStdString(),
    };

    emit LOG_D("Protocol: " + configValues->flash_protocol_selected_protocol_name, true, true);
    emit LOG_D("Make: " + configValues->flash_protocol_selected_make, true, true);
    emit LOG_D("Checksum: " + configValues->flash_protocol_selected_checksum, true, true);

    const fastecu::ui::ChecksumCorrectionResult result = m_checksumCorrectionCommand.run(
        bytes::view(ecuCalDef->FullRomData), ecuCalDef->use_romraider_definition,
        ecuCalDef->use_ecuflash_definition, selection, this);

    if (result.unknown_mcu_type)
    {
        emit LOG_E("Unknown MCU type: " + ecuCalDef->McuType, true, true);
        return;
    }
    if (result.canceled_due_to_missing_module)
    {
        emit LOG_D("Checksum calculation canceled!", true, true);
    }
    if (result.corrected_rom_data.has_value())
    {
        ecuCalDef->FullRomData = bytes::toQByteArray(bytes::ByteView(*result.corrected_rom_data));
    }
}
```

`MainWindow` declares its own `LOG_E` / `LOG_D` signals at `mainwindow.h:460` and `:463` with the identical `(QString message, bool timestamp, bool linefeed)` signature `FileActions` uses, so the emitted text carries over verbatim and no adaptation is needed. Declare `runChecksumCorrection` in `mainwindow.h` and include `src/algorithms/protocol/qt_bytes.h` (for `bytes::view` / `bytes::toQByteArray`) and `src/backend/checksum/checksum_selection.h`.

The two remaining `LOG_D` lines from the original method — `"ecuCalDef->McuType: " + ... + " " + configValues->flash_protocol_selected_mcu` and `"Size: 0x" + QString::number(ecuCalDef->FullRomData.length(), 16) + " -> 0x" + QString::number(device->romsize, 16)` (`file_actions.cpp:1693-1696`) — fire only on the known-MCU path and need the `flashdev_t *`. Reproduce them by calling `fastecu::flash::find_flash_device(selection.mcu_type)` in this helper before `run`, emitting both lines, and letting the command's own precheck handle the unknown case. Copy the exact expressions from `file_actions.cpp` rather than retyping them.

- [ ] **Step 4: Replace the three live call sites**

At lines 1144, 1645 and 1700, replace:

```cpp
                ecuCalDef[rom_number] = fileActions->checksum_correction(ecuCalDef[rom_number]);
```

with:

```cpp
                runChecksumCorrection(ecuCalDef[rom_number]);
```

The old form reassigned `ecuCalDef[rom_number]` from a method that always returned its own argument, so dropping the assignment is behaviour-preserving. Confirm the `if (ecuCalDef[rom_number] == nullptr)` check that follows the 1144 site still behaves — it tested a value that could never be null, and it is left exactly as-is.

- [ ] **Step 5: Delete the four commented call sites and the `menu_actions` line**

Remove the `/* */` blocks around lines 1630-1644 and 1685-1699 in `mainwindow.cpp` (they contain the superseded inline `QMessageBox` plus two dead calls each), and delete `src/ui/desktop/menu_actions.cpp:1545`.

- [ ] **Step 6: Delete the backend method and adapter**

In `src/backend/definitions/file_actions.h`, delete the `checksum_correction` declaration (line 212), the `checksumAdapter_` member, and the `#include "src/backend/checksum/legacy_checksum_adapter.h"`. In `file_actions.cpp`, delete the whole `FileActions::checksum_correction` body (lines 1673-1711).

```bash
git rm src/backend/checksum/legacy_checksum_adapter.h \
       src/backend/checksum/legacy_checksum_adapter.cpp \
       src/backend/checksum/legacy_checksum_adapter_test.cpp
```

In `src/backend/checksum/BUILD.bazel`, delete the `legacy_checksum_adapter` `qt_cc_library` and its `fastecu_gtest`, and remove the now-unused `COMMON_COPTS` / `QT_DEPS` / `qt_cc_library` / `fastecu_gtest` symbols from the `load` lines.

- [ ] **Step 7: Remove the three now-orphaned `FileActions` tests**

The three `checksum_correction_*` cases in `file_actions_parsing_test.cpp` — the two originals and Task 6's characterization — test a method that no longer exists. Delete them; their coverage lives in `checksum_correction_command_test.cpp` (the two behavioural cases, ported in Task 7) and in the `MainWindow` helper's log text, which Task 6 recorded and Step 3 reproduced.

- [ ] **Step 8: Verify `//src/backend/checksum` is Qt-free**

```bash
grep -rn "QT_DEPS" src/backend/checksum/BUILD.bazel
grep -rn "QMessageBox\|QString\|QWidget" src/backend/checksum/
bazel query 'somepath(//src/backend/checksum:dispatch, @qt_mac_aarch64//:lib)' 2>/dev/null
```

Expected: the first two print nothing; the query prints nothing.

- [ ] **Step 9: Confirm no `QMessageBox` remains anywhere in backend outside `definitions`**

```bash
grep -rn "QMessageBox\|QFileDialog" src/backend/ | grep -v "^src/backend/definitions/"
```

Expected: only the two documentation comments in `src/backend/ports/event_sink.h` and `src/backend/ports/file_repository.h`.

- [ ] **Step 10: Full gate**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Expected: all pass.

- [ ] **Step 11: Manually verify the dialogs still appear**

The seams are overridden in every test, so no automated test exercises the real `QMessageBox` path. Per the design's hardware-facing caution, check by hand:

```bash
bazel run --config=release //:fastecu
```

Open a ROM with a protocol whose `<checksum>` is `yes` and no definition linked; confirm the "WARNING! No definition file linked" dialog appears with both **OK** and **DO IT!** buttons, and that choosing **DO IT!** produces the "Checksums corrected" dialog. Record the outcome in the commit message.

- [ ] **Step 12: Commit**

```bash
git add -A
git commit -m "refactor(checksum): delete backend adapter, wire MainWindow to command (5e-2)

Removes LegacyChecksumAdapter, FileActions::checksum_correction and the
last QMessageBox in src/backend outside definitions. //src/backend/checksum
now carries no QT_DEPS.

Three live call sites, not seven: four of mainwindow.cpp's textual matches
were inside /* */ blocks and are deleted as dead code, along with the
superseded inline dialog they contained.

Dialogs verified by hand (no automated test covers the un-overridden seams)."
```

---

## Task 9: Update the documentation and close out the slice

**Files:**
- Modify: `docs/tech-debt.md`
- Modify: `docs/modularization-plan.md`
- Modify: `docs/superpowers/specs/2026-07-24-step5d-fileactions-decomposition-design.md`

**Interfaces:**
- Consumes: the completed state from Tasks 1-8.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Prove the completion criterion**

```bash
grep -n "src/backend" scripts/check-serial-compat-allowlist.py
grep -rn "QT_DEPS" src/backend/flash/BUILD.bazel src/backend/checksum/BUILD.bazel
bazel test --config=release //:serial_compat_allowlist //:portable_closure
```

Expected: the first two print nothing; the tests pass. This is the design's stated completion criterion — capture the output for the commit message.

- [ ] **Step 2: Update the tech-debt roadmap**

In `docs/tech-debt.md`:
- Under **"P1: Drain the `serial_qt_compat` allowlist"**: change "It currently holds 14 entries — 8 under `src/ui/desktop`, 2 in backend (`//src/backend/flash`, `//src/backend/logging/protocols`), plus …" to reflect 13 entries, 0 in backend, with `//src/platform/desktop/common/flash/legacy` now carrying the flash-family debt. Delete the first action bullet ("Split `FlashUtils::configureIso15765Can` out of `src/backend/flash/flash_utils.h` …") — done.
- Under **"P1: Finish the checksum UI boundary"**: the `LegacyChecksumAdapter` `QMessageBox` action is done. Reduce the section to the remaining item or delete it if nothing remains; per the file's own header, completed work is removed, not retained as a log.
- Under **"P1: Split `FileActions`"**: remove `checksum` from the list of what remains inside `FileActions`.

- [ ] **Step 3: Update the modularization plan**

In `docs/modularization-plan.md`, step 5's bullet list: record that 5e is complete and that "Remove direct `QMessageBox` … and `SerialPortActions` access from backend code" is discharged for `QMessageBox` and `SerialPortActions`, with `QFileDialog` and widget access remaining in `src/backend/definitions` as step-6 work. Update the step-5 status line in the **Status** section.

- [ ] **Step 4: Close the 5d umbrella's stale checkboxes**

`docs/superpowers/specs/2026-07-24-step5d-fileactions-decomposition-design.md`'s deliverable checklist still has 5d-5 and 5d-5b unchecked, though both merged (PRs #153 and #154). Check them, and add a line noting that step 5d is complete and 5e follows.

- [ ] **Step 5: Verify links**

Run: `prek run --files docs/tech-debt.md docs/modularization-plan.md docs/superpowers/specs/2026-07-24-step5d-fileactions-decomposition-design.md`
Expected: all hooks pass, including lychee. Cross-document references must be links with human-readable text, not backticked paths.

- [ ] **Step 6: Commit**

```bash
git add docs/
git commit -m "docs: record step 5e completion (5e)

Backend portability closure verified: zero //src/backend entries in the
serial_qt_compat allowlist (14 -> 13), and no QT_DEPS in
//src/backend/flash or //src/backend/checksum. Backend no longer depends
on src/platform.

Also closes 5d-5 / 5d-5b in the 5d umbrella's checklist -- both merged as
PRs #153 and #154."
```

---

## Follow-ups to file as issues (not carried in this work)

- **Retire the `findFlashDeviceIndex` Qt shim.** It exists only to keep the 30 legacy call sites on `QString`. Each tail family that migrates should convert its own sites to `fastecu::flash::find_flash_device_index`; delete the shim when the last one does.
- **Drain `//src/platform/desktop/common/flash/legacy:__pkg__` from the allowlist.** This is the step-5 tail's completion criterion, now attributed to the package that owns it.
- **`src/ui/desktop`'s `glob(["*.cpp"])` still has no `*_test.cpp` exclusion.** Task 7 sidestepped it with a new package rather than fixing it; tech-debt P2 tracks the general case across twelve packages.
