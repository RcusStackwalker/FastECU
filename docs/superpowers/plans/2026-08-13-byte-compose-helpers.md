# Byte-Frame Compose Helpers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace hand-rolled shift-and-mask frame building in the migrated flash executors with typed `composeBe*` helpers, and split `checksum8`'s boolean parameter into two named functions.

**Architecture:** A new Qt-free header `src/algorithms/protocol/bytes_compose.h` provides variadic `composeBe` / `composeBeWithExtraCapacity` / `composeBeWithChecksum`, where each argument's wire width is determined by its C++ type (`u24()` covers the width the type system cannot express, `_b` covers byte literals). `checksum8(ByteView, bool)` is deleted in favour of the existing `bytes::sum8` plus a new `checksum::negatedSum8`, which empties and therefore removes the `//src/algorithms/checksum:qt_compat` shim. The provably-unset `dec0x100` parameter leaves the SSM API.

**Tech Stack:** C++23, Bazel 9.1.1, GoogleTest via `fastecu_portable_gtest`, prek (clang-format/buildifier/lychee), clang-tidy.

**Spec:** [docs/superpowers/specs/2026-08-13-byte-compose-helpers-design.md](../specs/2026-08-13-byte-compose-helpers-design.md)

**Branch:** `markelov/byte-compose-helpers` (already created; the spec commit is its first commit)

## Global Constraints

- **C++23.** MSVC builds with `/std:c++latest`; macOS pins `--macos_minimum_os=26.0`.
- **`bytes_compose.h` must remain Qt-free.** It joins `//src/algorithms/protocol:protocol`, a portable target whose closure is checked by `//:portable_closure`. No new registration is needed in `BUILD.bazel`'s `genquery` or `PORTABLE_ROOTS`, but the guard must stay green.
- **Every header needs `#pragma once`** (enforced by prek).
- **Output must be byte-identical at every migrated site.** This is a behaviour-preserving refactor of code that writes to ECUs. No frame's length or content may change.
- **Do not use `static_assert(false, ...)` in a discarded `if constexpr` branch.** Use the dependent-false idiom; MSVC's P2593R1 support is newer than the rest of what this repo relies on.
- **Test files assert against hardcoded byte literals only**, never against a second computed form.
- Cross-document Markdown references are links with human-readable text, not backticked paths (lychee cannot see a path written as inline code).
- Commit after every task. Never commit to `master`.

## The width audit (read before Tasks 6-11)

**This is the single largest risk in the plan.** Under the width law, a value's wire width comes from its C++ type — but in this codebase several constants are declared wider than the field they are written into. Migrating by variable type instead of by observed byte count silently changes frame lengths.

Confirmed mismatches:

| Value | Declared type | Bytes actually emitted | Correct argument |
|---|---|---|---|
| `kReadPageSize` | `std::uint32_t` | 2 (`mc68hc16y5:478-479`, `sh7055:465-466`) | `std::uint16_t(kReadPageSize)` |
| `kCommitBlockSize` | `std::uint32_t` | 2 (`mc68hc16y5:680-681`, `sh7055:739-740`) | `std::uint16_t(kCommitBlockSize)` |
| `address` | `std::uint32_t` | 3 (`mc68hc16y5:377-379`, `sh7055:384-386`) | `u24(address)` |
| `block.length` | `std::uint32_t` | 3 (`mc68hc16y5:521-523`, `sh7055:516-518`) | `u24(block.length)` |
| `block.start` | `std::uint32_t` | 4 | `block.start` (no wrapper) |
| `numblocks`, `curblock` | `std::uint32_t` | 2 (`eeprom_kline:815-818`) | `std::uint16_t(numblocks)` |

**Mandatory procedure for every site in Tasks 6-11:** count the bytes the existing code emits, then choose the argument form to match that count. Never infer the width from the variable's declared type.

## Three shapes, not one

The 93 "hand-rolled shift" sites are not all frame composition. Three shapes appear, and only the first uses the new helpers:

- **Shape A — composition** (the large majority). Becomes `composeBe(...)`.
- **Shape B — indexed write of a decomposed value.** One site: `mc68hc16y5:369-370` writes `payload[2]`/`payload[3]` from `family_plan.kernel_magic`. Becomes `bytes::writeU16Be(payload, 2, family_plan.kernel_magic)` using the existing helper.
- **Shape C — constant decomposed for a comparison.** Three sites: `mc68hc16y5:67-68`, `denso_sh705x_eeprom_kline_executor.cpp:121-122`, `denso_sh705x_eeprom_can_executor.cpp:193-194`. Becomes `bytes::readU16Be(received, N) == kConstant` using the existing helper.

---

### Task 1: The compose header

**Files:**
- Create: `src/algorithms/protocol/bytes_compose.h`
- Create: `src/algorithms/protocol/bytes_compose_test.cpp`
- Modify: `src/algorithms/protocol/BUILD.bazel` (add header to `:protocol` hdrs; add test target; extend the `gtest_targets.bzl` load)

**Interfaces:**
- Consumes: `bytes::Byte`, `bytes::Bytes`, `bytes::ByteView`, `appendU16Be`, `appendU24Be`, `appendU32Be` from `src/algorithms/protocol/bytes.h`.
- Produces:
  - `struct bytes::U24 { std::uint32_t value; };`
  - `constexpr bytes::U24 bytes::u24(std::uint32_t value) noexcept`
  - `consteval bytes::Byte bytes::literals::operator""_b(unsigned long long)`
  - `template <typename T> concept bytes::ByteRange`
  - `template <typename... Args> bytes::Bytes bytes::composeBeWithExtraCapacity(std::size_t extra_capacity, const Args&... args)`
  - `template <typename... Args> bytes::Bytes bytes::composeBe(const Args&... args)`
  - `template <typename ChecksumFn, typename... Args> bytes::Bytes bytes::composeBeWithChecksum(ChecksumFn checksum, const Args&... args)`

- [ ] **Step 1: Write the failing test**

Create `src/algorithms/protocol/bytes_compose_test.cpp`:

```cpp
#include "src/algorithms/protocol/bytes_compose.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>

using bytes::composeBe;
using bytes::composeBeWithChecksum;
using bytes::composeBeWithExtraCapacity;
using bytes::u24;
using namespace bytes::literals;

// Rejected at compile time by the width law; kept as documentation because
// gtest cannot assert a static_assert failure:
//   composeBe(0x34);                      // int
//   composeBe('K');                       // char
//   composeBe(std::size_t{4});            // size_t
//   composeBe(0x1FF_b);                   // literal too wide for a byte

TEST(ComposeBe, EmitsOneByteForByte)
{
    EXPECT_EQ(composeBe(0x34_b), (bytes::Bytes{0x34}));
}

TEST(ComposeBe, EmitsTwoBytesForUint16MostSignificantFirst)
{
    EXPECT_EQ(composeBe(std::uint16_t{0xBEEF}), (bytes::Bytes{0xBE, 0xEF}));
}

TEST(ComposeBe, EmitsThreeBytesForU24MostSignificantFirst)
{
    EXPECT_EQ(composeBe(u24(0x123456)), (bytes::Bytes{0x12, 0x34, 0x56}));
}

TEST(ComposeBe, TruncatesU24ToItsLowThreeBytes)
{
    EXPECT_EQ(composeBe(u24(0xFF123456)), (bytes::Bytes{0x12, 0x34, 0x56}));
}

TEST(ComposeBe, EmitsFourBytesForUint32MostSignificantFirst)
{
    EXPECT_EQ(composeBe(std::uint32_t{0x12345678}), (bytes::Bytes{0x12, 0x34, 0x56, 0x78}));
}

TEST(ComposeBe, SplicesByteRangesInline)
{
    const bytes::Bytes payload{0xAA, 0xBB};
    const std::array<bytes::Byte, 2> tail{0xCC, 0xDD};
    EXPECT_EQ(composeBe(0x01_b, bytes::ByteView(payload), tail),
              (bytes::Bytes{0x01, 0xAA, 0xBB, 0xCC, 0xDD}));
}

TEST(ComposeBe, AppendsStringViewCharsAsBytes)
{
    EXPECT_EQ(composeBe(std::string_view{"KERN2"}),
              (bytes::Bytes{0x4B, 0x45, 0x52, 0x4E, 0x32}));
}

TEST(ComposeBe, EmitsNothingForNoArguments)
{
    EXPECT_EQ(composeBe(), bytes::Bytes{});
}

TEST(ComposeBe, AppendsArgumentsLeftToRight)
{
    EXPECT_EQ(composeBe(0x34_b, u24(0x00A000), 0x04_b, u24(0x000200)),
              (bytes::Bytes{0x34, 0x00, 0xA0, 0x00, 0x04, 0x00, 0x02, 0x00}));
}

TEST(ComposeBeWithExtraCapacity, ReservesWithoutEmitting)
{
    const bytes::Bytes out = composeBeWithExtraCapacity(4, 0x01_b, 0x02_b);
    EXPECT_EQ(out, (bytes::Bytes{0x01, 0x02}));
    EXPECT_GE(out.capacity(), out.size() + 4);
}

TEST(ComposeBeWithChecksum, AppendsOneByteForByteReturningFunction)
{
    const auto sum = [](bytes::ByteView data) {
        bytes::Byte total = 0;
        for (const bytes::Byte value : data)
        {
            total = static_cast<bytes::Byte>(total + value);
        }
        return total;
    };
    // 0x80 + 0x10 = 0x90; + 0xF0 = 0x180 -> 0x80; + 0x01 = 0x81.
    EXPECT_EQ(composeBeWithChecksum(sum, 0x80_b, 0x10_b, 0xF0_b, 0x01_b),
              (bytes::Bytes{0x80, 0x10, 0xF0, 0x01, 0x81}));
}

TEST(ComposeBeWithChecksum, AppendsFourBytesForUint32ReturningFunction)
{
    const auto fixed = [](bytes::ByteView) { return std::uint32_t{0x5AA5A55A}; };
    EXPECT_EQ(composeBeWithChecksum(fixed, 0x01_b),
              (bytes::Bytes{0x01, 0x5A, 0xA5, 0xA5, 0x5A}));
}

TEST(ComposeBeWithChecksum, ComputesChecksumOverEverythingComposed)
{
    const auto count = [](bytes::ByteView data) { return bytes::Byte(data.size()); };
    const bytes::Bytes payload{0xAA, 0xBB, 0xCC};
    EXPECT_EQ(composeBeWithChecksum(count, std::uint16_t{0xBEEF}, bytes::ByteView(payload)),
              (bytes::Bytes{0xBE, 0xEF, 0xAA, 0xBB, 0xCC, 0x05}));
}

// The SSM header shape, asserted against literal hex rather than recomputed.
TEST(ComposeBeWithChecksum, BuildsTheSsmHeaderFrame)
{
    const auto sum = [](bytes::ByteView data) {
        bytes::Byte total = 0;
        for (const bytes::Byte value : data)
        {
            total = static_cast<bytes::Byte>(total + value);
        }
        return total;
    };
    const bytes::Bytes payload{0xEF, 0x52};
    const bytes::Bytes framed = composeBeWithChecksum(
        sum, 0x80_b, 0x10_b, 0xF0_b, bytes::Byte(payload.size()), bytes::ByteView(payload));
    EXPECT_EQ(framed, (bytes::Bytes{0x80, 0x10, 0xF0, 0x02, 0xEF, 0x52, 0xC3}));
}
```

- [ ] **Step 2: Add the Bazel wiring**

In `src/algorithms/protocol/BUILD.bazel`, change the load line to pull in both macros:

```python
load("//bazel:gtest_targets.bzl", "fastecu_gtest", "fastecu_portable_gtest")
```

Change the `:protocol` target's `hdrs`:

```python
    hdrs = [
        "bytes.h",
        "bytes_compose.h",
    ],
```

Add the test target after the existing `bytes_test`:

```python
fastecu_portable_gtest(
    name = "bytes_compose_test",
    srcs = ["bytes_compose_test.cpp"],
    deps = [":protocol"],
)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `bazel test --config=release //src/algorithms/protocol:bytes_compose_test`
Expected: FAIL — compilation error, `bytes_compose.h` does not exist.

- [ ] **Step 4: Write the header**

Create `src/algorithms/protocol/bytes_compose.h`:

```cpp
#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace bytes
{

// 24-bit field marker. uint32_t alone cannot say 3 bytes vs 4, and both
// widths occur in the ECU frame formats this composes.
struct U24
{
    std::uint32_t value;
};

constexpr U24 u24(std::uint32_t value) noexcept
{
    return U24{value};
}

namespace literals
{

// 0x34_b -> Byte. consteval, so an out-of-range literal is a compile error
// rather than a silent truncation: a throw-expression is not a constant
// expression, so the call cannot be evaluated.
consteval Byte operator""_b(unsigned long long value)
{
    return value <= 0xFF ? static_cast<Byte>(value)
                         : throw "byte literal does not fit in one byte";
}

} // namespace literals

template <typename T>
concept ByteRange =
    std::ranges::input_range<T> && std::same_as<std::ranges::range_value_t<T>, Byte>;

namespace detail
{

// Deliberately not `static_assert(false, ...)`: P2593R1 support in MSVC is
// newer than the rest of what this repo relies on, and MSVC builds with
// /std:c++latest.
template <typename>
inline constexpr bool dependentFalse = false;

template <typename T>
constexpr std::size_t widthBe(const T& arg)
{
    using U = std::remove_cvref_t<T>;
    if constexpr (std::same_as<U, Byte>)
    {
        return 1;
    }
    else if constexpr (std::same_as<U, std::uint16_t>)
    {
        return 2;
    }
    else if constexpr (std::same_as<U, U24>)
    {
        return 3;
    }
    else if constexpr (std::same_as<U, std::uint32_t>)
    {
        return 4;
    }
    else if constexpr (std::same_as<U, std::string_view>)
    {
        return arg.size();
    }
    else if constexpr (ByteRange<U>)
    {
        if constexpr (std::ranges::sized_range<U>)
        {
            return std::ranges::size(arg);
        }
        else
        {
            return 0; // capacity is a hint, never correctness
        }
    }
    else
    {
        return 0;
    }
}

template <typename T>
void appendBe(Bytes& out, const T& arg)
{
    using U = std::remove_cvref_t<T>;
    if constexpr (std::same_as<U, Byte>)
    {
        out.push_back(arg);
    }
    else if constexpr (std::same_as<U, std::uint16_t>)
    {
        appendU16Be(out, arg);
    }
    else if constexpr (std::same_as<U, U24>)
    {
        appendU24Be(out, arg.value);
    }
    else if constexpr (std::same_as<U, std::uint32_t>)
    {
        appendU32Be(out, arg);
    }
    else if constexpr (std::same_as<U, std::string_view>)
    {
        for (const char character : arg)
        {
            out.push_back(static_cast<Byte>(character));
        }
    }
    else if constexpr (ByteRange<U>)
    {
        out.insert(out.end(), std::ranges::begin(arg), std::ranges::end(arg));
    }
    else
    {
        static_assert(dependentFalse<U>,
                      "composeBe: argument must be Byte, std::uint16_t, u24(), "
                      "std::uint32_t, std::string_view, or a range of Byte. A bare "
                      "integer literal is an int -- write 0x34_b instead.");
    }
}

} // namespace detail

// Composes `args` big-endian, reserving `extra_capacity` bytes beyond the
// composed length so a caller that appends afterwards does not reallocate.
template <typename... Args>
Bytes composeBeWithExtraCapacity(std::size_t extra_capacity, const Args&... args)
{
    Bytes out;
    out.reserve(extra_capacity + (std::size_t{0} + ... + detail::widthBe(args)));
    (detail::appendBe(out, args), ...);
    return out;
}

template <typename... Args>
Bytes composeBe(const Args&... args)
{
    return composeBeWithExtraCapacity(0, args...);
}

// Composes `args` big-endian, then appends `checksum(composed)` using the
// same width law -- a Byte-returning function appends one byte, a
// uint32_t-returning one appends four, most-significant first.
template <typename ChecksumFn, typename... Args>
Bytes composeBeWithChecksum(ChecksumFn checksum, const Args&... args)
{
    using Sum = std::invoke_result_t<ChecksumFn, ByteView>;
    static_assert(std::unsigned_integral<Sum>,
                  "composeBeWithChecksum: checksum function must return an unsigned integer");
    Bytes out = composeBeWithExtraCapacity(sizeof(Sum), args...);
    detail::appendBe(out, static_cast<Sum>(checksum(ByteView(out))));
    return out;
}

} // namespace bytes
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `bazel test --config=release //src/algorithms/protocol:bytes_compose_test`
Expected: PASS, 14 tests.

- [ ] **Step 6: Verify the portable guard and formatting**

Run:
```bash
bazel test --config=release //:portable_closure
prek run --files src/algorithms/protocol/bytes_compose.h src/algorithms/protocol/bytes_compose_test.cpp src/algorithms/protocol/BUILD.bazel
```
Expected: both PASS.

- [ ] **Step 7: Commit**

```bash
git add src/algorithms/protocol/bytes_compose.h src/algorithms/protocol/bytes_compose_test.cpp src/algorithms/protocol/BUILD.bazel
git commit -m "feat: add typed big-endian byte-frame compose helpers"
```

---

### Task 2: Rename the three-argument `sum8`

`bytes::sum8` is currently an overload set. Passing an overload set where the callable is a deduced template parameter is an ambiguity error, so `composeBeWithChecksum(bytes::sum8, ...)` would not compile until this rename lands.

**Files:**
- Modify: `src/algorithms/protocol/bytes.h` (the three-argument `sum8`)
- Modify: `src/algorithms/protocol/mut_dma/mut_dma_codec.cpp:8-11`
- Modify: `src/algorithms/protocol/mut_dma/mut_dma_codec.h:19`

**Interfaces:**
- Produces: `bytes::Byte bytes::sum8Range(ByteView bytes, std::size_t from, std::size_t len)`. `bytes::sum8(ByteView)` survives unchanged and is now a single function.

- [ ] **Step 1: Write the failing test**

Add to `src/algorithms/protocol/bytes_test.cpp`:

```cpp
TEST(TestBytes, sum8Range_sumsOnlyTheRequestedWindow)
{
    const bytes::Bytes buf{0x01, 0x02, 0x03, 0x04};
    EXPECT_EQ(bytes::sum8Range(bytes::ByteView(buf), 1, 2), static_cast<bytes::Byte>(0x05));
}

TEST(TestBytes, sum8Range_clampsLengthToWhatIsAvailable)
{
    const bytes::Bytes buf{0x01, 0x02, 0x03, 0x04};
    EXPECT_EQ(bytes::sum8Range(bytes::ByteView(buf), 2, 99), static_cast<bytes::Byte>(0x07));
}

TEST(TestBytes, sum8_isPassableAsACallable)
{
    bytes::Byte (*fn)(bytes::ByteView) = bytes::sum8;
    const bytes::Bytes buf{0x01, 0x02};
    EXPECT_EQ(fn(bytes::ByteView(buf)), static_cast<bytes::Byte>(0x03));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/algorithms/protocol:bytes_test`
Expected: FAIL — `sum8Range` is not declared, and the callable conversion is ambiguous.

- [ ] **Step 3: Rename in `bytes.h`**

In `src/algorithms/protocol/bytes.h`, rename the three-argument overload and leave the one-argument function alone:

```cpp
// Sums `len` bytes starting at `from`, clamping `len` to what is available.
// Named distinctly from sum8(ByteView) so that sum8 remains a single
// function and can therefore be passed as a callable (see
// composeBeWithChecksum in bytes_compose.h).
inline Byte sum8Range(ByteView bytes, std::size_t from, std::size_t len)
{
    if (from >= bytes.size())
    {
        return 0;
    }
    const auto slice = bytes.subspan(from, std::min(len, bytes.size() - from));
    const auto sum = std::accumulate(slice.begin(), slice.end(), 0u);
    return static_cast<Byte>(sum & 0xFF);
}

inline Byte sum8(ByteView bytes)
{
    return sum8Range(bytes, 0, bytes.size());
}
```

- [ ] **Step 4: Update the one caller**

`mut_dma_codec.cpp:10` is the only caller of the three-argument `bytes::sum8` in the tree. Change its body only:

```cpp
bytes::Byte sum8(bytes::ByteView bytes, std::size_t from, std::size_t len)
{
    return bytes::sum8Range(bytes, from, len);
}
```

**Do not rename the mut_dma-namespace `sum8` wrapper.** Its declaration in `mut_dma_codec.h:19` keeps its current name and signature, and its callers — `mut_dma_codec.cpp:35` and `:55`, `mut_dma_freeform.cpp:50`, `mut_dma_test.cpp:36`, `codec_test.cpp:24`, `freeform_test.cpp:46` — are all untouched.

The rename exists solely so that `bytes::sum8` stops being an overload set and can be passed as a callable to `composeBeWithChecksum`. The mut_dma wrapper is never passed as a callable, so renaming it would churn six sites across four files and change a header API for no benefit.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bazel test --config=release //src/algorithms/protocol:all //src/algorithms/protocol/mut_dma:all`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/algorithms/protocol/bytes.h src/algorithms/protocol/bytes_test.cpp src/algorithms/protocol/mut_dma/mut_dma_codec.h src/algorithms/protocol/mut_dma/mut_dma_codec.cpp
git commit -m "refactor: rename three-argument bytes::sum8 to sum8Range"
```

---

### Task 3: Split `checksum8` and delete the Qt checksum shim

`checksum8` is removed in the same task as its 36 call sites and the shim it backs, because deleting the function without its callers leaves the tree unbuildable.

**Files:**
- Modify: `src/algorithms/checksum/checksum_primitives.h:26-31`, `checksum_primitives.cpp:77-81`
- Modify: `src/algorithms/checksum/checksum_primitives_test.cpp:60-72`
- Delete: `src/algorithms/checksum/qt_checksum.h`, `qt_checksum.cpp`, `qt_checksum_test.cpp`
- Modify: `src/algorithms/checksum/BUILD.bazel` (remove `:qt_compat` and `test_qt_checksum`)
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel:50`
- Modify (portable callers): `src/backend/flash/ecu/subaru_denso_sh7055_02_executor.cpp:59,391,393,615`, `subaru_denso_mc68hc16y5_02_executor.cpp:60,383`, `src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.cpp:115`, `src/algorithms/protocol/ssm/ssm_protocol_core.cpp:91,117`, `src/backend/logging/protocols/ssm_logging_protocol_test.cpp:45`, `src/backend/flash/ecu/subaru_denso_sh7055_02_executor_test.cpp:195`, `subaru_denso_mc68hc16y5_02_executor_test.cpp:158`, `src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor_test.cpp:120`, `src/platform/desktop/common/flash/flash_worker_test.cpp:98`
- Modify (legacy Qt callers): `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp:250`, `flash_ecu_subaru_hitachi_sh7058_can_operation.cpp:844`, `flash_ecu_subaru_denso_sh705x_kline_operation.cpp` (11 sites: 523, 783, 852, 891, 941, 1018, 1101, 1154, 1251, 1643), `legacy/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp:651`, `legacy/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp:226,379,501`

**Interfaces:**
- Consumes: `bytes::sum8(ByteView)` from Task 2.
- Produces: `std::uint8_t fastecu::checksum::negatedSum8(bytes::ByteView data)`. `fastecu::checksum::checksum8` no longer exists in any form.

- [ ] **Step 1: Write the failing test**

Replace the two `Checksum8` tests at `src/algorithms/checksum/checksum_primitives_test.cpp:60-72` with:

```cpp
TEST(NegatedSum8, ComplementsTheSumAgainst0x100)
{
    const bytes::Bytes payload{0xA8, 0x00, 0x11, 0x22, 0x33};
    EXPECT_EQ(fastecu::checksum::negatedSum8(bytes::ByteView(payload)), bytes::Byte(0xF2));
}

TEST(NegatedSum8, YieldsZeroForAZeroSum)
{
    const bytes::Bytes payload{0x00, 0x00};
    EXPECT_EQ(fastecu::checksum::negatedSum8(bytes::ByteView(payload)), bytes::Byte(0x00));
}

TEST(NegatedSum8, IsThePlainSumSubtractedFrom0x100)
{
    const bytes::Bytes frame{0x80, 0x10, 0xF0, 0x01, 0xBF};
    const bytes::Byte plain = bytes::sum8(bytes::ByteView(frame));
    EXPECT_EQ(fastecu::checksum::negatedSum8(bytes::ByteView(frame)),
              static_cast<bytes::Byte>(0x100 - plain));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/algorithms/checksum:checksum_primitives_test`
Expected: FAIL — `negatedSum8` is not declared.

- [ ] **Step 3: Replace the function**

In `src/algorithms/checksum/checksum_primitives.h`, delete the `checksum8` declaration and its comment (lines 26-31) and put in their place:

```cpp
// Additive 8-bit checksum complemented as 0x100 - sum, the framing
// convention used by the Subaru/Denso kernel-upload envelopes. The plain
// (uncomplemented) sum is bytes::sum8.
std::uint8_t negatedSum8(bytes::ByteView data);
```

In `src/algorithms/checksum/checksum_primitives.cpp`, replace the `checksum8` definition at lines 77-81 with:

```cpp
std::uint8_t negatedSum8(bytes::ByteView data)
{
    return static_cast<std::uint8_t>(0x100 - bytes::sum8(data));
}
```

- [ ] **Step 4: Migrate the three production `true` callers**

`src/backend/flash/ecu/subaru_denso_sh7055_02_executor.cpp:391` and `:393`:

```cpp
    request[7] = fastecu::checksum::negatedSum8(request);
    request.insert(request.end(), encrypted.begin(), encrypted.end());
    request.push_back(fastecu::checksum::negatedSum8(request));
```

`src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.cpp:383`:

```cpp
    request.push_back(fastecu::checksum::negatedSum8(request));
```

- [ ] **Step 5: Migrate the plain callers to `bytes::sum8`**

Each of these becomes `bytes::sum8(<view>)`. Where the argument is already a `bytes::Bytes` or `ByteView` no conversion is needed:

- `subaru_denso_sh7055_02_executor.cpp:59` → `out.push_back(bytes::sum8(out));`
- `subaru_denso_sh7055_02_executor.cpp:615` → `response.back() != bytes::sum8(bytes::ByteView(response).first(response.size() - 1))`
- `subaru_denso_mc68hc16y5_02_executor.cpp:60` → `out.push_back(bytes::sum8(out));`
- `denso_sh705x_eeprom_kline_executor.cpp:115` → `out.push_back(bytes::sum8(out));`
- `ssm_logging_protocol_test.cpp:45`, `subaru_denso_sh7055_02_executor_test.cpp:195`, `subaru_denso_mc68hc16y5_02_executor_test.cpp:158`, `denso_sh705x_eeprom_kline_executor_test.cpp:120`, `flash_worker_test.cpp:98` → `out.push_back(bytes::sum8(out));` (matching each file's local variable name)
- `checksum_primitives_test.cpp:63` is replaced by the tests in Step 1.

`ssm_protocol_core.cpp:91` and `:117` still thread the `dec0x100` variable, which Task 4 removes. They cannot be left calling the deleted `checksum8`, or the tree will not build at the end of this task. Bridge them with a ternary that preserves current behaviour exactly:

```cpp
    // :91, inside addHeader
    framed.push_back(dec0x100 ? fastecu::checksum::negatedSum8(framed) : bytes::sum8(framed));

    // :117, inside hasValidFrame
    const bytes::ByteView body = frame.first(frame.size() - checksumLength);
    return (dec0x100 ? fastecu::checksum::negatedSum8(body) : bytes::sum8(body)) ==
           frame[frame.size() - checksumLength];
```

Task 4 collapses both to plain `bytes::sum8` when it removes the parameter.

Add `#include "src/algorithms/protocol/bytes.h"` to any of these files that does not already include it, and drop the now-unused `checksum_primitives.h` include where nothing else in the file uses it.

- [ ] **Step 6: Migrate the five legacy Qt callers**

Each site changes from `fastecu::checksum::checksum8(output, false)` to `bytes::sum8(bytes::view(output))`, and the file's `#include "src/algorithms/checksum/qt_checksum.h"` is replaced with:

```cpp
#include "src/algorithms/protocol/qt_bytes.h"
```

Files and sites:
- `legacy/ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp:250`
- `legacy/ecu/flash_ecu_subaru_hitachi_sh7058_can_operation.cpp:844`
- `legacy/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp:523, 783, 852, 891, 941, 1018, 1101, 1154, 1251, 1643`
- `legacy/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp:651` (single-argument form; same replacement)
- `legacy/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp:226, 379, 501` — also update the commented-out line at `:228` so the comment does not reference a function that no longer exists

- [ ] **Step 7: Delete the shim and its Bazel targets**

```bash
git rm src/algorithms/checksum/qt_checksum.h src/algorithms/checksum/qt_checksum.cpp src/algorithms/checksum/qt_checksum_test.cpp
```

In `src/algorithms/checksum/BUILD.bazel`, delete the `qt_compat` `cc_library` (including its `# TRANSITIONAL Qt shim` comment) and the `test_qt_checksum` `fastecu_gtest` target. Remove the now-unused loads if nothing else in the file uses them — check whether `fastecu_gtest`, `COMMON_COPTS`, and `QT_DEPS` still have consumers before deleting their `load` statements.

In `src/platform/desktop/common/flash/legacy/BUILD.bazel:50`, remove the `"//src/algorithms/checksum:qt_compat",` dep and confirm `"//src/algorithms/protocol:qt_compat"` is present in the same `deps` list (add it if it is not).

- [ ] **Step 8: Run the full suite**

Run: `bazel test --config=release //...`
Expected: PASS. No target named `//src/algorithms/checksum:qt_compat` should remain; `bazel query //src/algorithms/checksum:all` must not list it.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "refactor: split checksum8 into bytes::sum8 and checksum::negatedSum8

Deletes the //src/algorithms/checksum:qt_compat shim, whose only content
was the QByteArray overload of checksum8."
```

---

### Task 4: Remove `dec0x100` from the SSM API

**Files:**
- Modify: `src/algorithms/protocol/ssm/ssm_protocol_core.h`, `ssm_protocol_core.cpp:79-120`
- Modify: `src/algorithms/protocol/ssm/ssm_protocol.h:18-23`, `ssm_protocol.cpp:23-37`
- Modify: 66 call sites passing `, false)` across `src/platform/desktop/common/flash/legacy/**`, `src/backend/flash/**`, and test files

**Interfaces:**
- Consumes: `bytes::composeBeWithChecksum`, `bytes::literals::operator""_b` (Task 1); `bytes::sum8` as a single callable (Task 2).
- Produces:
  - `bytes::Bytes SsmProtocol::addHeader(bytes::ByteView output, bytes::Byte testerId, bytes::Byte targetId)`
  - `bool SsmProtocol::hasValidFrame(bytes::ByteView frame, bytes::Byte receiverId, bytes::Byte senderId)`
  - `bool SsmProtocol::hasPayloadPrefix(bytes::ByteView frame, bytes::ByteView prefix, bytes::Byte receiverId, bytes::Byte senderId)`
  - Qt shim overloads in `ssm_protocol.h` with the same parameter lists, taking `QByteArray`.

- [ ] **Step 1: Write the failing test**

Add to `src/algorithms/protocol/ssm/ssm_protocol_core_test.cpp`:

```cpp
TEST(SsmProtocolCore, AddHeaderBuildsTheFramedRequest)
{
    const bytes::Bytes payload{0xEF, 0x52};
    EXPECT_EQ(SsmProtocol::addHeader(bytes::ByteView(payload), 0xF0, 0x10),
              (bytes::Bytes{0x80, 0x10, 0xF0, 0x02, 0xEF, 0x52, 0xC3}));
}

TEST(SsmProtocolCore, HasValidFrameAcceptsWhatAddHeaderProduces)
{
    const bytes::Bytes payload{0xEF, 0x52};
    const bytes::Bytes framed = SsmProtocol::addHeader(bytes::ByteView(payload), 0xF0, 0x10);
    EXPECT_TRUE(SsmProtocol::hasValidFrame(bytes::ByteView(framed), 0x10, 0xF0));
}

TEST(SsmProtocolCore, HasValidFrameRejectsACorruptedChecksum)
{
    bytes::Bytes framed{0x80, 0x10, 0xF0, 0x02, 0xEF, 0x52, 0xC3};
    framed.back() = 0xC4;
    EXPECT_FALSE(SsmProtocol::hasValidFrame(bytes::ByteView(framed), 0x10, 0xF0));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/algorithms/protocol/ssm:all`
Expected: FAIL — the three-argument `addHeader` does not exist (the current one takes four).

- [ ] **Step 3: Rewrite the core**

In `src/algorithms/protocol/ssm/ssm_protocol_core.cpp`, add the include:

```cpp
#include "src/algorithms/protocol/bytes_compose.h"
```

Replace `addHeader` (lines 79-93):

```cpp
bytes::Bytes addHeader(bytes::ByteView output, bytes::Byte testerId, bytes::Byte targetId)
{
    using namespace bytes::literals;
    return bytes::composeBeWithChecksum(bytes::sum8, 0x80_b, targetId, testerId,
                                        bytes::Byte(output.size()), output);
}
```

In `hasValidFrame`, drop the `dec0x100` parameter and replace the final return (line 117) with an explicit comparison — verification is not composition, and routing it through the compose helper would let a bug in the helper hide itself:

```cpp
    return bytes::sum8(frame.first(frame.size() - checksumLength)) ==
           frame[frame.size() - checksumLength];
```

Drop the `dec0x100` parameter from `hasPayloadPrefix` and from its internal `hasValidFrame` call. Update the three declarations in `ssm_protocol_core.h` to match.

- [ ] **Step 4: Update the Qt shim**

In `src/algorithms/protocol/ssm/ssm_protocol.h:18-23` and `ssm_protocol.cpp:23-37`, drop the `bool dec0x100 = false` parameter from all three declarations and definitions, and drop the argument from each forwarding call.

- [ ] **Step 5: Remove the argument at all 66 call sites**

Every remaining compile error is a call passing an explicit `, false)`. Find them with:

```bash
grep -rn "addHeader(\|hasValidFrame(\|hasPayloadPrefix(" src/ apps/ tests/ | grep ", false)"
```

Delete the `, false` from each. Expect 66 sites, the bulk in `src/platform/desktop/common/flash/legacy/tcu/` and `legacy/ecu/`. The 23 sites that relied on the default argument need no edit.

Also update the two comments that document the flag — `src/backend/flash/flash_executor.h:84` and `src/backend/flash/flash_types.h:184` both explain when callers "need this OFF (false, the default)". Reword them to describe the framing itself, since the knob is gone.

- [ ] **Step 6: Run the full suite**

Run: `bazel test --config=release //...`
Expected: PASS. Then confirm the flag is gone:

```bash
grep -rn "dec0x100" src/ apps/ tests/
```
Expected: no output.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "refactor: drop the unset dec0x100 parameter from the SSM API

No caller ever passed true. addHeader now builds its frame with
composeBeWithChecksum."
```

---

### Tasks 5-10: per-file migration

**All six tasks follow the same three-phase sequencing rule from the spec.** It exists because the test files hand-roll their expected frames today, which makes them an independent second derivation of each wire format. Migrating production first and running the *unmigrated* tests is what proves byte-identity; only then may the test file be migrated.

**Every one of these tasks:**

1. Migrate the production file. Apply the width audit at every site — count the bytes the existing code emits, never infer width from the variable's declared type.
2. Run the package's tests **without touching them**. A green run is the byte-identity proof. If anything fails, the migration changed the wire format: fix the production code, do not adjust the test.
3. Commit the production change on its own.
4. Migrate the test file's hand-rolled expectations.
5. Run the package's tests again.
6. Commit the test change.

Add to each migrated file:

```cpp
#include "src/algorithms/protocol/bytes_compose.h"
```

and, inside the file's anonymous namespace, `using namespace bytes::literals;`.

---

### Task 5: `subaru_hitachi_m32r_kline_executor.cpp`

Smallest file; do it first to establish the pattern.

**Files:**
- Modify: `src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor.cpp:305-310, 401-402`
- Test (unchanged, used as the proof): `src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor_test.cpp`

**Interfaces:**
- Consumes: `bytes::composeBe`, `bytes::u24`, `bytes::literals::operator""_b` (Task 1).
- Produces: nothing new; behaviour is unchanged.

- [ ] **Step 1: Migrate the read request (lines 305-310)**

`address` is `std::uint32_t` and emits **3** bytes, so it takes `u24`. `p.chunk_size` emits 1.

```cpp
        auto response = exchange(transport, cancellation,
                                 composeBe(0xa0_b, 0x00_b, 0x00_b, u24(address),
                                           bytes::Byte(p.chunk_size - 1)),
                                 p);
```

- [ ] **Step 2: Migrate the write request (lines 401-404)**

```cpp
        const bytes::Bytes request = composeBe(
            0x36_b, u24(address),
            bytes::ByteView(encrypted).subspan(address, p.chunk_size));
```

This replaces both the brace-initialised prefix and the following `request.insert(...)` call, so delete the `insert` line.

- [ ] **Step 3: Run the package tests unmigrated**

Run: `bazel test --config=release //src/backend/flash/ecu:all`
Expected: PASS. This is the byte-identity proof.

- [ ] **Step 4: Commit**

```bash
git add src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor.cpp
git commit -m "refactor: compose Hitachi M32R K-Line frames with composeBe"
```

- [ ] **Step 5: Check the test file**

Run: `grep -nE "static_cast<bytes::Byte>\(.* >> " src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor_test.cpp`
Expected: no output — this test file hand-rolls no frames, so there is nothing to migrate and no second commit for this task.

---

### Task 6: `subaru_mitsu_m32r_kline_executor.cpp`

**Files:**
- Modify: `src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor.cpp:208-210, 273-276`
- Test (unchanged, used as the proof): `src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor_test.cpp`

**Interfaces:**
- Consumes: `bytes::composeBe`, `bytes::u24`, `bytes::literals::operator""_b` (Task 1).
- Produces: nothing new; behaviour is unchanged.

- [ ] **Step 1: Migrate the read request (lines 208-210)**

`address` emits **3** bytes.

```cpp
        const bytes::Bytes request = composeBe(0xa0_b, 0x00_b, 0x20_b, u24(address),
                                               bytes::Byte(p.chunk_size - 1));
```

- [ ] **Step 2: Migrate the write request (lines 273-276)**

```cpp
        const bytes::Bytes request = composeBe(
            0x36_b, u24(address),
            bytes::ByteView(encrypted).subspan(address, p.chunk_size));
```

Delete the following `request.insert(...)` call, which this absorbs.

- [ ] **Step 3: Run the package tests unmigrated**

Run: `bazel test --config=release //src/backend/flash/ecu:all`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor.cpp
git commit -m "refactor: compose Mitsubishi M32R K-Line frames with composeBe"
```

- [ ] **Step 5: Check the test file**

Run: `grep -nE "static_cast<bytes::Byte>\(.* >> " src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor_test.cpp`
Expected: no output. Nothing to migrate.

---

### Task 7: `denso_sh705x_eeprom_kline_executor.cpp`

11 production sites, 8 test sites.

**Files:**
- Modify: `src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.cpp:76-118, 121-122, 274-278, 812-819`
- Modify (phase 2): `src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor_test.cpp`

**Interfaces:**
- Consumes: `bytes::composeBe`, `bytes::composeBeWithChecksum`, `bytes::u24`, `bytes::literals::operator""_b` (Task 1); `bytes::sum8` (Task 2).
- Produces: nothing new; behaviour is unchanged.

- [ ] **Step 1: Migrate `sid_27_send_key_request` (lines 77-82)**

```cpp
bytes::Bytes sid_27_send_key_request(bytes::ByteView key)
{
    return composeBe(0x27_b, 0x02_b, key);
}
```

- [ ] **Step 2: Migrate `sid_34_request` (lines 87-99)**

Both `addr` and `len` emit **3** bytes each.

```cpp
bytes::Bytes sid_34_request(std::uint32_t addr, std::uint32_t len)
{
    return composeBe(0x34_b, u24(addr), 0x04_b, u24(len));
}
```

- [ ] **Step 3: Migrate `request_kernel_id_frame` (lines 106-117)**

`kSubKernelStartComm` is `std::uint16_t` and emits 2 bytes. The literal `1` in the original stands for `datalen(0) + 1` and emits 2 bytes, so it becomes `std::uint16_t{1}`.

```cpp
bytes::Bytes request_kernel_id_frame()
{
    return composeBeWithChecksum(bytes::sum8, kSubKernelStartComm, std::uint16_t{1},
                                 kSubKernelId);
}
```

Keep the existing comment noting that this frame is not `SsmProtocol::addHeader`-framed.

- [ ] **Step 4: Migrate the Shape C comparison (lines 121-122)**

```cpp
    return received.size() > 4 && bytes::readU16Be(received, 0) == kSubKernelStartComm &&
           received[4] == static_cast<bytes::Byte>(kSubKernelId | 0x40);
```

- [ ] **Step 5: Migrate the block-address request (lines 274-278)**

Read the surrounding lines first to confirm the emitted byte count for `block_addr`; the two visible shift lines plus a mask line indicate **3** bytes, so use `u24(block_addr)`.

- [ ] **Step 6: Migrate the dump request (lines 812-819)**

`numblocks` and `curblock` are `std::uint32_t` but each emits **2** bytes:

```cpp
        const bytes::Bytes request = composeBe(kSidDump, bytes::Byte(mode),
                                               std::uint16_t(numblocks),
                                               std::uint16_t(curblock));
```

- [ ] **Step 7: Run the package tests unmigrated**

Run: `bazel test --config=release //src/backend/flash/eeprom:all`
Expected: PASS. This is the byte-identity proof for all six edits.

- [ ] **Step 8: Commit the production change**

```bash
git add src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.cpp
git commit -m "refactor: compose Denso SH705x EEPROM K-Line frames with composeBe"
```

- [ ] **Step 9: Migrate the test file's hand-rolled expectations**

In `denso_sh705x_eeprom_kline_executor_test.cpp`, find the 8 sites with:

```bash
grep -n ">> 8) & 0xFF\|>> 16) & 0xFF\|>> 24) & 0xFF" src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor_test.cpp
```

Convert each to `composeBe`, applying the same width audit. Sites at `:73` and `:102` also use `out.insert(out.end(), ...)` after a brace-initialised prefix; those collapse into a single `composeBe` call with the range as a trailing argument.

- [ ] **Step 10: Run the package tests**

Run: `bazel test --config=release //src/backend/flash/eeprom:all`
Expected: PASS.

- [ ] **Step 11: Commit the test change**

```bash
git add src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor_test.cpp
git commit -m "test: compose expected EEPROM K-Line frames with composeBe"
```

---

### Task 8: `denso_sh705x_eeprom_can_executor.cpp`

22 production sites, 12 test sites.

**Files:**
- Modify: `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.cpp:56-66, 96-118, 121-160, 172-186, 193-195, 270-280, 754-761`
- Modify (phase 2): `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor_test.cpp`

**Interfaces:**
- Consumes: `bytes::composeBe`, `bytes::u24`, `bytes::literals::operator""_b` (Task 1).
- Produces: nothing new; behaviour is unchanged.

- [ ] **Step 1: Migrate `can_frame` (lines 56-67)**

`request_id` is `std::uint32_t` and emits **4** bytes, so it needs no wrapper:

```cpp
bytes::Bytes can_frame(std::uint32_t request_id, bytes::ByteView payload)
{
    return composeBe(request_id, payload);
}
```

- [ ] **Step 2: Migrate `seed_key_send_request` (lines 96-101)**

```cpp
bytes::Bytes seed_key_send_request(std::uint32_t request_id, bytes::ByteView key)
{
    return can_frame(request_id, composeBe(0x27_b, 0x02_b, key));
}
```

- [ ] **Step 3: Leave `session_set_request` (lines 108-118) alone**

It builds its payload conditionally with two `if` statements. `composeBe` takes a fixed argument list and cannot express that. Leave the `push_back` calls and keep the existing comment explaining why both flags are computed dynamically.

- [ ] **Step 4: Migrate `request_kernel_id_frame` (lines 121-130)**

`kSubKernelStartComm` emits 2 bytes; the `0x00, 0x01` pair is a 2-byte length field:

```cpp
    return can_frame(request_id, composeBe(kSubKernelStartComm, std::uint16_t{1}, kSubKernelId));
```

Keep the comment recording that this frame, unlike its K-Line sibling, carries no trailing checksum.

- [ ] **Step 5: Migrate the remaining request builders (lines 136-160, 172-186)**

Apply the width audit at each. `addr` and `data_len` emit **3** bytes each (`u24`); `pagesize` emits **2** (`std::uint16_t(...)`); `block_addr` emits **3** (`u24`). Read each brace-initialiser and count before converting.

- [ ] **Step 6: Migrate the Shape C comparison (lines 193-195)**

```cpp
    return received.size() > 8 && bytes::readU16Be(received, 4) == kSubKernelStartComm &&
           received[8] == static_cast<bytes::Byte>(kSubKernelId | 0x40);
```

- [ ] **Step 7: Migrate the decrypted-word composition (lines 273-279)**

`decrypted` emits **4** bytes.

- [ ] **Step 8: Replace the kernel checksum word (lines 757-760)**

This is not a compose site — the buffer is built by `resize`, not from an argument list. Replace the four `push_back` calls with the existing helper:

```cpp
    bytes::appendU32Be(buf, chk_sum);
```

- [ ] **Step 9: Run the package tests unmigrated**

Run: `bazel test --config=release //src/backend/flash/eeprom:all`
Expected: PASS.

- [ ] **Step 10: Commit the production change**

```bash
git add src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.cpp
git commit -m "refactor: compose Denso SH705x EEPROM CAN frames with composeBe"
```

- [ ] **Step 11: Migrate the test file**

12 sites. The kernel-id expectation at `:288-292` pushes `'K'`, `'E'`, `'R'`, `'N'`, `'2'` one character at a time; `char` is rejected by the width law, so use the `std::string_view` form:

```cpp
    out = composeBe(out, std::string_view{"KERN2"});
```

The four `push_back` calls at `:271-274` become a single `bytes::appendU32Be(plEncr, chkSum);`.

- [ ] **Step 12: Run the package tests**

Run: `bazel test --config=release //src/backend/flash/eeprom:all`
Expected: PASS.

- [ ] **Step 13: Commit the test change**

```bash
git add src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor_test.cpp
git commit -m "test: compose expected EEPROM CAN frames with composeBe"
```

---

### Task 9: `subaru_denso_sh7055_02_executor.cpp`

24 production sites, 12 test sites.

**Files:**
- Modify: `src/backend/flash/ecu/subaru_denso_sh7055_02_executor.cpp:51-61, 382-393, 460-467, 509-519, 667-673, 700-706, 733-745`
- Modify (phase 2): `src/backend/flash/ecu/subaru_denso_sh7055_02_executor_test.cpp`

**Interfaces:**
- Consumes: `bytes::composeBe`, `bytes::composeBeWithChecksum`, `bytes::u24`, `bytes::literals::operator""_b` (Task 1); `bytes::sum8` (Task 2); `checksum::negatedSum8` (Task 3).
- Produces: nothing new; behaviour is unchanged.

- [ ] **Step 1: Migrate `frame` (lines 51-61)**

`kStartComm` is `std::uint16_t` (2 bytes) and `length` is already `std::uint16_t` (2 bytes):

```cpp
bytes::Bytes frame(std::uint8_t opcode, bytes::ByteView payload = {})
{
    const std::uint16_t length = static_cast<std::uint16_t>(payload.size() + 1);
    return composeBeWithChecksum(bytes::sum8, kStartComm, length, bytes::Byte(opcode), payload);
}
```

Also update the wire-shape comment three lines above (`subaru_denso_sh7055_02_executor.cpp:50`), which currently ends `[payload][checksum8]`. `checksum8` no longer exists, and Task 12 greps for that identifier expecting no hits:

```cpp
// [0xBE][0xEF][length high][length low][opcode][payload][sum8].
```

- [ ] **Step 2: Leave the kernel-upload envelope (lines 382-393) hand-rolled**

This site patches a checksum into the middle of the frame (`request[7] = negatedSum8(request)`) and then appends a second one over the extended buffer. `composeBeWithChecksum` cannot express it. Convert only the leading `bytes::Bytes request{...}` brace-initialiser at lines 383-386 — where `address` emits **3** bytes — and add a comment above the two `negatedSum8` calls:

```cpp
    // Not composeBeWithChecksum: the first checksum is patched into the
    // middle of the frame at offset 7, and the second covers the frame plus
    // the encrypted payload appended afterwards.
```

- [ ] **Step 3: Migrate the read request (lines 460-467)**

`address` emits **3** bytes (`u24`). `kReadPageSize` is declared `std::uint32_t` but emits **2** — write `std::uint16_t(kReadPageSize)`.

- [ ] **Step 4: Migrate the CRC request payload (lines 509-519)**

`block.start` emits **4** bytes (plain `std::uint32_t`); `block.length` emits **3** (`u24`).

- [ ] **Step 5: Migrate the remaining three sites (lines 667-673, 700-706, 733-745)**

`block.start` and `chunk_address` and `commit_block_start` emit **4** bytes each; `kCommitBlockSize` is declared `std::uint32_t` but emits **2** (`std::uint16_t(kCommitBlockSize)`); `commit_crc` emits **4**.

- [ ] **Step 6: Run the package tests unmigrated**

Run: `bazel test --config=release //src/backend/flash/ecu:all`
Expected: PASS.

- [ ] **Step 7: Commit the production change**

```bash
git add src/backend/flash/ecu/subaru_denso_sh7055_02_executor.cpp
git commit -m "refactor: compose Denso SH7055 frames with composeBe"
```

- [ ] **Step 8: Migrate the test file (12 sites)**

Run: `grep -n ">> 8) & 0xFF\|>> 16) & 0xFF\|>> 24) & 0xFF" src/backend/flash/ecu/subaru_denso_sh7055_02_executor_test.cpp`

Convert each with the width audit applied.

- [ ] **Step 9: Run the package tests**

Run: `bazel test --config=release //src/backend/flash/ecu:all`
Expected: PASS.

- [ ] **Step 10: Commit the test change**

```bash
git add src/backend/flash/ecu/subaru_denso_sh7055_02_executor_test.cpp
git commit -m "test: compose expected SH7055 frames with composeBe"
```

---

### Task 10: `subaru_denso_mc68hc16y5_02_executor.cpp`

29 production sites, 8 test sites. Largest file; do it last.

**Files:**
- Modify: `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.cpp:52-62, 65-69, 365-383, 473-481, 514-524, 615-621, 646-653, 674-686`
- Modify (phase 2): `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor_test.cpp`

**Interfaces:**
- Consumes: `bytes::composeBe`, `bytes::composeBeWithChecksum`, `bytes::u24`, `bytes::literals::operator""_b` (Task 1); `bytes::sum8` (Task 2); `checksum::negatedSum8` (Task 3).
- Produces: nothing new; behaviour is unchanged.

- [ ] **Step 1: Migrate `frame` (lines 52-62)**

```cpp
bytes::Bytes frame(std::uint8_t opcode, bytes::ByteView payload = {})
{
    const std::uint16_t len_plus_one = static_cast<std::uint16_t>(payload.size() + 1);
    return composeBeWithChecksum(bytes::sum8, kStartComm, len_plus_one, bytes::Byte(opcode),
                                 payload);
}
```

Also update the wire-shape comment three lines above (`subaru_denso_mc68hc16y5_02_executor.cpp:50`), which currently ends `[payload][checksum8]`. `checksum8` no longer exists, and Task 12 greps for that identifier expecting no hits:

```cpp
// [0xBE][0xEF][len+1 hi][len+1 lo][opcode][payload][sum8].
```

- [ ] **Step 2: Migrate the Shape C comparison (lines 65-69)**

```cpp
    return received.size() > 5 && bytes::readU16Be(received, 0) == kStartComm &&
           received[4] == expected_opcode_with_ack;
}
```

- [ ] **Step 3: Migrate the Shape B indexed write (lines 369-370)**

`family_plan.kernel_magic` is written into `payload[2]` and `payload[3]` as a 2-byte big-endian field:

```cpp
    bytes::writeU16Be(payload, 2, family_plan.kernel_magic);
```

Confirm `payload` converts to `bytes::MutableByteView`; if it is a `bytes::Bytes`, pass `bytes::MutableByteView(payload)`.

- [ ] **Step 4: Leave the kernel-upload envelope (lines 374-383) partly hand-rolled**

Same shape as the SH7055 sibling. Convert the leading brace-initialiser only — `address` emits **3** bytes, `length` emits **3** — and leave the trailing `negatedSum8` append with an explanatory comment.

- [ ] **Step 5: Migrate the read request (lines 473-481)**

`address` emits **3** bytes; `kReadPageSize` is `std::uint32_t` but emits **2** — `std::uint16_t(kReadPageSize)`.

- [ ] **Step 6: Migrate the CRC request payload (lines 514-524)**

`block.start` emits **4**; `block.length` emits **3**.

- [ ] **Step 7: Migrate the remaining three sites (lines 615-621, 646-653, 674-686)**

`block.start`, `chunk_address`, `commit_block_start`, and `commit_crc` each emit **4** bytes; `kCommitBlockSize` emits **2** despite its `std::uint32_t` declaration.

- [ ] **Step 8: Run the package tests unmigrated**

Run: `bazel test --config=release //src/backend/flash/ecu:all`
Expected: PASS.

- [ ] **Step 9: Commit the production change**

```bash
git add src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.cpp
git commit -m "refactor: compose Denso MC68HC16Y5 frames with composeBe"
```

- [ ] **Step 10: Migrate the test file (8 sites)**

Run: `grep -n ">> 8) & 0xFF\|>> 16) & 0xFF\|>> 24) & 0xFF" src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor_test.cpp`

Convert each with the width audit applied.

- [ ] **Step 11: Run the package tests**

Run: `bazel test --config=release //src/backend/flash/ecu:all`
Expected: PASS.

- [ ] **Step 12: Commit the test change**

```bash
git add src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor_test.cpp
git commit -m "test: compose expected MC68HC16Y5 frames with composeBe"
```

---

### Task 11: ADR 0013 and the last hand-composed frame

**Files:**
- Create: `docs/adr/0013-compose-byte-frames.md`
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp:127-131`
- Modify: `CLAUDE.md` (the ADR list in "Writing targets and tests")

**Interfaces:**
- Consumes: `bytes::composeBe` (Task 1).
- Produces: nothing new.

**Note:** the `mitsu_colt_m32r_can_executor.cpp` change is a small deliberate extension beyond the spec's six files. `build_request` hand-composes a frame with `appendU32Be` plus `insert`; leaving the one obvious hand-composed frame in the tree immediately after adding the helper would undercut the ADR. It contains no shift-and-mask sites, which is why it fell outside the spec's count of 93.

- [ ] **Step 1: Migrate `build_request`**

```cpp
bytes::Bytes build_request(std::uint32_t request_id, bytes::ByteView payload)
{
    return bytes::composeBe(request_id, payload);
}
```

Keep the existing comment about the request id being threaded from the plan rather than hardcoded.

- [ ] **Step 2: Run the package tests**

Run: `bazel test --config=release //src/backend/flash/ecu:all`
Expected: PASS.

- [ ] **Step 3: Write the ADR**

Create `docs/adr/0013-compose-byte-frames.md`, following the structure of [ADR 0012](../../adr/0012-use-std-ranges.md):

```markdown
# 13. Compose byte frames with composeBe

Date: 2026-08-13

## Status

Accepted

## Context

Protocol frames were built by hand-rolling endianness — 93 shift-and-mask
sites across six migrated flash executors — even though `bytes.h` already
provided `appendU16Be` / `appendU24Be` / `appendU32Be`. The idiom is easy to
get wrong in a way the compiler cannot catch, and these frames are written
to ECUs, where a wrong length or byte order is a bricking risk.

A second pattern recurred on top of it: compose several fixed-width values,
then append a checksum computed over what was just composed.

## Decision

Build byte frames with `bytes::composeBe`, `bytes::composeBeWithExtraCapacity`,
and `bytes::composeBeWithChecksum` from
[bytes_compose.h](../../src/algorithms/protocol/bytes_compose.h).

Each argument's wire width comes from its C++ type: `Byte` emits one byte,
`std::uint16_t` two, `u24(x)` three, `std::uint32_t` four, and any range of
`Byte` splices inline. Byte literals are written with the `_b` suffix. Any
other type is a compile error, which is what stops a `std::size_t` from
silently emitting eight bytes.

A value declared wider than its wire field must be narrowed explicitly at the
call site — `std::uint16_t(kReadPageSize)`, `u24(address)`. Count the bytes
the frame needs; do not infer them from the variable's declared type.

## Consequences

Frame construction reads as a declaration of the wire format. Endianness bugs
become compile errors rather than silent truncations.

Two shapes stay hand-rolled, deliberately:

- A checksum patched into the middle of a frame and a second appended over the
  extended buffer (the SH7055 and MC68HC16Y5 kernel-upload envelopes).
- A checksum over a buffer built by `resize` rather than from an argument list
  (the EEPROM CAN kernel word), which uses `bytes::appendU32Be` directly.

Verification code does not use the helpers. `hasValidFrame` compares an
explicit `bytes::sum8` against the received checksum, so that a bug in the
compose helper cannot cancel itself out on both sides of the comparison.

There are no little-endian variants. Every wire format in this repository is
big-endian, and the little-endian append helpers added earlier on symmetry
grounds never acquired a production caller.
```

- [ ] **Step 4: Add the ADR to CLAUDE.md**

In the "Writing targets and tests" section, extend the ADR list sentence to include the new one, matching the existing style:

```markdown
- Prefer `std::string_view` by value over `const char*` / `const std::string&` (ADR 0009), gmock matchers for property assertions (ADR 0010), `std::format` for message construction (ADR 0011), ranges/views over index loops (ADR 0012), and `bytes::composeBe` over hand-rolled shift-and-mask frame building (ADR 0013).
```

- [ ] **Step 5: Verify links and formatting**

Run: `prek run --all-files`
Expected: PASS, including lychee (the ADR's relative links must resolve).

- [ ] **Step 6: Commit**

```bash
git add docs/adr/0013-compose-byte-frames.md CLAUDE.md src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp
git commit -m "docs: add ADR 0013 for composing byte frames with composeBe"
```

---

### Task 12: Final verification sweep

**Files:** none modified unless a gate fails.

**Interfaces:**
- Consumes: everything from Tasks 1-11.
- Produces: a green tree ready for a pull request.

- [ ] **Step 1: Confirm the hand-rolled sites are gone**

```bash
grep -rn ">> 8) & 0xFF\|>> 16) & 0xFF\|>> 24) & 0xFF" src/backend/flash/ src/algorithms/protocol/
```
Expected: only the deliberately-retained sites — the SH7055 and MC68HC16Y5 kernel-upload envelopes, and any site whose retention is explained by an adjacent comment. Anything else is an unfinished migration.

```bash
grep -rn "checksum8\|dec0x100" src/ apps/ tests/
```
Expected: no output.

- [ ] **Step 2: Run the full test suite**

Run: `bazel test --config=release //...`
Expected: PASS, all targets.

If `//tests:serial_backend_tests` fails on Windows, that is a known pre-existing intermittent crash unrelated to this work — rerun it before investigating.

- [ ] **Step 3: Run the build-graph guards explicitly**

```bash
bazel test --config=release //:portable_closure //:serial_compat_allowlist //:openpty_includes //:bazel_openssl_wiring
```
Expected: PASS.

- [ ] **Step 4: Run the lint gates**

```bash
prek run --all-files
bazel run //:clang_tidy_report_changed
```
Expected: both clean. Fix anything reported, then rerun.

- [ ] **Step 5: Confirm the release build links**

Run: `bazel build --config=release //:fastecu`
Expected: success.

- [ ] **Step 6: Commit any fixes and push**

```bash
git status                      # expect a clean tree if no gate needed fixing
git push -u origin markelov/byte-compose-helpers
```

Then open a pull request against `master`. Do not push without the user's authorization.

---

## Self-review

**Spec coverage.** Every section of the spec maps to a task: the new header and vocabulary and the three compose functions to Task 1; the `sum8Range` rename to Task 2; `negatedSum8`, the `checksum8` deletion and the Qt shim removal to Task 3; the SSM `dec0x100` removal and the `addHeader` rewrite to Task 4; the six production files and their four test files to Tasks 5-10 under the spec's sequencing rule; the two deliberately-hand-rolled sites to Tasks 9 and 10 with comments, and to the ADR's Consequences; ADR 0013 to Task 11; the verification gates and hardware-safety claim to Task 12.

**Two refinements the spec did not anticipate**, both recorded above rather than left for the implementer to discover:

- The 93 sites comprise three shapes, not one. Four of them (`mc68hc16y5:67-68` and `:369-370`, `eeprom_kline:121-122`, `eeprom_can:193-194`) are comparisons and an indexed write, which take the existing `readU16Be` and `writeU16Be` rather than `composeBe`.
- Several constants are declared wider than the field they are written into — `kReadPageSize` and `kCommitBlockSize` are `std::uint32_t` but emit two bytes. Migrating by declared type would silently change frame lengths, so the width audit is a mandatory per-site step.

**Type consistency.** `composeBe`, `composeBeWithExtraCapacity`, `composeBeWithChecksum`, `u24`, `U24`, `ByteRange`, `operator""_b`, `sum8`, `sum8Range`, and `negatedSum8` are spelled identically in every task that produces or consumes them.
