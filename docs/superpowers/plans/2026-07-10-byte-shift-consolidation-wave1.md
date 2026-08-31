# Byte-Shift Consolidation (Wave 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add BE/LE fixed-width (16/24/32-bit) byte-array read/write helpers to `protocol/bytes.h` and `protocol/qt_bytes.h`, then convert every manual shift-based byte-array read/write call site that has existing regression-test coverage to use them, removing the now-fully-redundant dead `EcuOperations` byte/int helpers and `ssm_protocol.cpp`'s local duplicate along the way.

**Architecture:** `protocol/bytes.h` stays Qt-free and span-based (mirrors its existing `readU16/24/32Be`/`appendU16/24/32Be`/`sum8` shape). `protocol/qt_bytes.h` adds thin `QByteArray`-native overloads on top, since most legacy call sites build `QByteArray` directly. Every other task is a mechanical, behavior-preserving substitution at existing call sites, verified either by an existing byte-exact unit test or by a Bazel compile of `//:fastecu_core_common` (the shared library every call site here already builds into).

**Tech Stack:** C++20 (`std::span`, `std::array`), Qt (`QByteArray`), Bazel, QTest.

## Global Constraints

- Behavior-preserving only: every converted call site must produce byte-identical output to before. No new features, no changed defaults.
- Follow the approved spec: `docs/superpowers/specs/2026-07-10-byte-shift-consolidation-design.md`.
- Scope for this plan (Wave 1) is only files with existing regression-test coverage. The ~19 untested `modules/ecu/flash_ecu_subaru_*` / `modules/tcu/flash_tcu_subaru_*` / `modules/eeprom/eeprom_ecu_subaru_*` per-variant files are **out of scope** — Wave 2, a separate plan written after this one lands. The ~80 protocol-constant-splitting response comparisons (e.g. `received.at(4) != ((CONST>>8)&0xFF) || ...`) approved in the spec all live inside this same untested Tier 4 file set, so they are deferred to Wave 2 as well — none of them appear in this plan's 21 tasks.
- Every task that touches `ecu_operations.cpp`, `dtc_operations.cpp`, `get_key_operations_subaru.cpp`, or `serial_port_actions_direct.cpp` has **no dedicated behavioral test** for the exact lines changed; its verification step is `bazel build //:fastecu_core_common` (confirms compilation across the whole shared library) plus the algebraic-equivalence note inline in the task. This mirrors the precedent set by the prior `cks_add8` consolidation, which touched several of the same untested classes.
- Commit after each task (`git add` the exact files touched; do not use `git add -A`).

---

### Task 1: `protocol/bytes.h` — add LE read/append and BE/LE in-place write, plus new `test_bytes` suite

**Files:**
- Modify: `protocol/bytes.h`
- Create: `tests/test_bytes.h`, `tests/test_bytes.cpp`, `tests/test_bytes_main.cpp`
- Modify: `bazel/fastecu_sources.bzl` (add `tests/test_bytes.cpp` to `MUT_DMA_TESTS_COMMON_SRCS`, `tests/test_bytes.h` to `MUT_DMA_TESTS_COMMON_HDRS`)
- Modify: `bazel/mut_dma_test_suites.bzl` (add `"test_bytes"` to `MUT_DMA_TEST_SUITES`)
- Modify: `tests/tests.pro` (add `test_bytes.cpp` to `SOURCES`, `test_bytes.h` to `HEADERS`)
- Modify: `tests/main.cpp` (add `#include "test_bytes.h"` and `run_test_bytes(argc, argv)`)

**Interfaces:**
- Produces (in namespace `bytes`, `protocol/bytes.h`):
  - `std::uint16_t readU16Le(ByteView bytes, std::size_t offset = 0)`
  - `std::uint32_t readU24Le(ByteView bytes, std::size_t offset = 0)`
  - `std::uint32_t readU32Le(ByteView bytes, std::size_t offset = 0)`
  - `void appendU16Le(Bytes& out, std::uint16_t value)`
  - `void appendU24Le(Bytes& out, std::uint32_t value)`
  - `void appendU32Le(Bytes& out, std::uint32_t value)`
  - `using MutableByteView = std::span<Byte>;`
  - `void writeU16Be(MutableByteView out, std::size_t offset, std::uint16_t value)`
  - `void writeU24Be(MutableByteView out, std::size_t offset, std::uint32_t value)`
  - `void writeU32Be(MutableByteView out, std::size_t offset, std::uint32_t value)`
  - `void writeU16Le(MutableByteView out, std::size_t offset, std::uint16_t value)`
  - `void writeU24Le(MutableByteView out, std::size_t offset, std::uint32_t value)`
  - `void writeU32Le(MutableByteView out, std::size_t offset, std::uint32_t value)`
  - `int run_test_bytes(int argc, char **argv)` (declared in `tests/test_bytes.h`)

- [ ] **Step 1: Write the new test file `tests/test_bytes.h`**

```cpp
#ifndef TEST_BYTES_H
#define TEST_BYTES_H
int run_test_bytes(int argc, char **argv);
#endif
```

- [ ] **Step 2: Write the new test file `tests/test_bytes_main.cpp`**

```cpp
#include "test_bytes.h"

int main(int argc, char **argv)
{
    return run_test_bytes(argc, argv);
}
```

- [ ] **Step 3: Write the failing test file `tests/test_bytes.cpp`**

```cpp
#include <QtTest>

#include <array>
#include <cstdint>

#include "protocol/bytes.h"
#include "test_bytes.h"

class TestBytes : public QObject
{
    Q_OBJECT
  private slots:
    void readU16Le_readsLowByteFirst()
    {
        const std::array<bytes::Byte, 2> data{0x34, 0x12};
        QCOMPARE(bytes::readU16Le(bytes::ByteView(data)), std::uint16_t(0x1234));
    }

    void readU24Le_readsLowByteFirst()
    {
        const std::array<bytes::Byte, 3> data{0x56, 0x34, 0x12};
        QCOMPARE(bytes::readU24Le(bytes::ByteView(data)), std::uint32_t(0x123456));
    }

    void readU32Le_readsLowByteFirst()
    {
        const std::array<bytes::Byte, 4> data{0x78, 0x56, 0x34, 0x12};
        QCOMPARE(bytes::readU32Le(bytes::ByteView(data)), std::uint32_t(0x12345678));
    }

    void readU16Le_respectsOffset()
    {
        const std::array<bytes::Byte, 4> data{0xFF, 0xFF, 0x34, 0x12};
        QCOMPARE(bytes::readU16Le(bytes::ByteView(data), 2), std::uint16_t(0x1234));
    }

    void readLe_outOfBoundsReturnsZero()
    {
        const std::array<bytes::Byte, 1> data{0x12};
        QCOMPARE(bytes::readU16Le(bytes::ByteView(data)), std::uint16_t(0));
        QCOMPARE(bytes::readU24Le(bytes::ByteView(data)), std::uint32_t(0));
        QCOMPARE(bytes::readU32Le(bytes::ByteView(data)), std::uint32_t(0));
    }

    void appendU16Le_appendsLowByteFirst()
    {
        bytes::Bytes out;
        bytes::appendU16Le(out, 0x1234);
        QCOMPARE(out, (bytes::Bytes{0x34, 0x12}));
    }

    void appendU24Le_appendsLowByteFirst()
    {
        bytes::Bytes out;
        bytes::appendU24Le(out, 0x123456);
        QCOMPARE(out, (bytes::Bytes{0x56, 0x34, 0x12}));
    }

    void appendU32Le_appendsLowByteFirst()
    {
        bytes::Bytes out;
        bytes::appendU32Le(out, 0x12345678);
        QCOMPARE(out, (bytes::Bytes{0x78, 0x56, 0x34, 0x12}));
    }

    void writeU16Be_writesAtOffsetWithoutDisturbingRest()
    {
        std::array<bytes::Byte, 4> data{0xAA, 0xAA, 0xAA, 0xAA};
        bytes::writeU16Be(bytes::MutableByteView(data), 1, 0x1234);
        QCOMPARE(data, (std::array<bytes::Byte, 4>{0xAA, 0x12, 0x34, 0xAA}));
    }

    void writeU24Be_writesBigEndianBytes()
    {
        std::array<bytes::Byte, 3> data{};
        bytes::writeU24Be(bytes::MutableByteView(data), 0, 0x123456);
        QCOMPARE(data, (std::array<bytes::Byte, 3>{0x12, 0x34, 0x56}));
    }

    void writeU32Be_writesBigEndianBytes()
    {
        std::array<bytes::Byte, 4> data{};
        bytes::writeU32Be(bytes::MutableByteView(data), 0, 0x12345678);
        QCOMPARE(data, (std::array<bytes::Byte, 4>{0x12, 0x34, 0x56, 0x78}));
    }

    void writeU16Le_writesLittleEndianBytes()
    {
        std::array<bytes::Byte, 2> data{};
        bytes::writeU16Le(bytes::MutableByteView(data), 0, 0x1234);
        QCOMPARE(data, (std::array<bytes::Byte, 2>{0x34, 0x12}));
    }

    void writeU24Le_writesLittleEndianBytes()
    {
        std::array<bytes::Byte, 3> data{};
        bytes::writeU24Le(bytes::MutableByteView(data), 0, 0x123456);
        QCOMPARE(data, (std::array<bytes::Byte, 3>{0x56, 0x34, 0x12}));
    }

    void writeU32Le_writesLittleEndianBytes()
    {
        std::array<bytes::Byte, 4> data{};
        bytes::writeU32Le(bytes::MutableByteView(data), 0, 0x12345678);
        QCOMPARE(data, (std::array<bytes::Byte, 4>{0x78, 0x56, 0x34, 0x12}));
    }

    void writeBe_outOfBoundsIsNoOp()
    {
        std::array<bytes::Byte, 1> data{0xAA};
        bytes::writeU16Be(bytes::MutableByteView(data), 0, 0x1234);
        QCOMPARE(data, (std::array<bytes::Byte, 1>{0xAA}));
    }
};

int run_test_bytes(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestBytes t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_bytes.moc"
```

- [ ] **Step 4: Wire the new suite into the build**

In `bazel/fastecu_sources.bzl`, add `"tests/test_bytes.cpp",` to `MUT_DMA_TESTS_COMMON_SRCS` (alphabetically, next to `"tests/test_ssm_protocol.cpp",`) and `"tests/test_bytes.h",` to `MUT_DMA_TESTS_COMMON_HDRS` (alphabetically).

In `bazel/mut_dma_test_suites.bzl`, add `"test_bytes",` to the `MUT_DMA_TEST_SUITES` list (next to `"test_ssm_protocol",`).

In `tests/tests.pro`, add `test_bytes.cpp \` to the `SOURCES +=` block (next to `test_ssm_protocol.cpp \`) and `test_bytes.h \` to the `HEADERS +=` block (next to `test_ssm_protocol.h \`).

In `tests/main.cpp`, add `#include "test_bytes.h"` (next to `#include "test_ssm_protocol.h"`) and `status |= run_test_bytes(argc, argv);` (next to the `run_test_ssm_protocol` line).

- [ ] **Step 5: Run the new test to verify it fails (functions not yet defined)**

Run: `bazel test //tests:test_bytes --test_output=errors`
Expected: FAIL to build — `readU16Le`/`readU24Le`/`readU32Le`/`appendU16Le`/`appendU24Le`/`appendU32Le`/`writeU16Be`/`writeU24Be`/`writeU32Be`/`writeU16Le`/`writeU24Le`/`writeU32Le`/`MutableByteView` are not members of `bytes`.

- [ ] **Step 6: Add the new functions to `protocol/bytes.h`**

Insert after the existing `appendU32Be` (after line 36) and before `readU16Be`:

```cpp
inline void appendU16Le(Bytes& out, std::uint16_t value)
{
    out.push_back(static_cast<Byte>(value & 0xFF));
    out.push_back(static_cast<Byte>((value >> 8) & 0xFF));
}

inline void appendU24Le(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<Byte>(value & 0xFF));
    out.push_back(static_cast<Byte>((value >> 8) & 0xFF));
    out.push_back(static_cast<Byte>((value >> 16) & 0xFF));
}

inline void appendU32Le(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<Byte>(value & 0xFF));
    out.push_back(static_cast<Byte>((value >> 8) & 0xFF));
    out.push_back(static_cast<Byte>((value >> 16) & 0xFF));
    out.push_back(static_cast<Byte>((value >> 24) & 0xFF));
}
```

Insert after the existing `readU32Be` (after line 57) and before `sum8`:

```cpp
inline std::uint16_t readU16Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset + 2 > bytes.size())
        return 0;
    return static_cast<std::uint16_t>(std::uint16_t(bytes[offset]) | (std::uint16_t(bytes[offset + 1]) << 8));
}

inline std::uint32_t readU24Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset + 3 > bytes.size())
        return 0;
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) | (std::uint32_t(bytes[offset + 2]) << 16);
}

inline std::uint32_t readU32Le(ByteView bytes, std::size_t offset = 0)
{
    if (offset + 4 > bytes.size())
        return 0;
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) | (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
}

using MutableByteView = std::span<Byte>;

inline void writeU16Be(MutableByteView out, std::size_t offset, std::uint16_t value)
{
    if (offset + 2 > out.size())
        return;
    out[offset] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 1] = static_cast<Byte>(value & 0xFF);
}

inline void writeU24Be(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset + 3 > out.size())
        return;
    out[offset] = static_cast<Byte>((value >> 16) & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<Byte>(value & 0xFF);
}

inline void writeU32Be(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset + 4 > out.size())
        return;
    out[offset] = static_cast<Byte>((value >> 24) & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 16) & 0xFF);
    out[offset + 2] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 3] = static_cast<Byte>(value & 0xFF);
}

inline void writeU16Le(MutableByteView out, std::size_t offset, std::uint16_t value)
{
    if (offset + 2 > out.size())
        return;
    out[offset] = static_cast<Byte>(value & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
}

inline void writeU24Le(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset + 3 > out.size())
        return;
    out[offset] = static_cast<Byte>(value & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
}

inline void writeU32Le(MutableByteView out, std::size_t offset, std::uint32_t value)
{
    if (offset + 4 > out.size())
        return;
    out[offset] = static_cast<Byte>(value & 0xFF);
    out[offset + 1] = static_cast<Byte>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<Byte>((value >> 16) & 0xFF);
    out[offset + 3] = static_cast<Byte>((value >> 24) & 0xFF);
}
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `bazel test //tests:test_bytes --test_output=errors`
Expected: PASS, all 15 test slots green.

- [ ] **Step 8: Commit**

```bash
git add protocol/bytes.h tests/test_bytes.h tests/test_bytes.cpp tests/test_bytes_main.cpp \
        bazel/fastecu_sources.bzl bazel/mut_dma_test_suites.bzl tests/tests.pro tests/main.cpp
git commit -m "feat: add LE read/append and BE/LE in-place write helpers to bytes.h"
```

---

### Task 2: `protocol/qt_bytes.h` — QByteArray-native append/write overloads

**Files:**
- Modify: `protocol/qt_bytes.h`
- Modify: `tests/test_bytes.cpp` (append test cases)

**Interfaces:**
- Consumes: `bytes::MutableByteView`, `bytes::writeU16Be/24Be/32Be/16Le/24Le/32Le` (Task 1)
- Produces (namespace `bytes`, `protocol/qt_bytes.h`):
  - `MutableByteView mutableView(QByteArray& bytes)`
  - `void appendU16Be(QByteArray& out, std::uint16_t value)` / `appendU24Be` / `appendU32Be`
  - `void appendU16Le(QByteArray& out, std::uint16_t value)` / `appendU24Le` / `appendU32Le`
  - `void writeU16Be(QByteArray& out, std::size_t offset, std::uint16_t value)` / `writeU24Be` / `writeU32Be`
  - `void writeU16Le(QByteArray& out, std::size_t offset, std::uint16_t value)` / `writeU24Le` / `writeU32Le`

- [ ] **Step 1: Add failing test cases to `tests/test_bytes.cpp`**

Add these slots to the `TestBytes` class (needs `#include "protocol/qt_bytes.h"` added to the top of the file alongside the existing `#include "protocol/bytes.h"`):

```cpp
    void qByteArrayAppendU32Be_matchesSpanVersion()
    {
        QByteArray out;
        bytes::appendU32Be(out, 0x12345678);
        QCOMPARE(out, QByteArray::fromHex("12345678"));
    }

    void qByteArrayAppendU32Le_matchesSpanVersion()
    {
        QByteArray out;
        bytes::appendU32Le(out, 0x12345678);
        QCOMPARE(out, QByteArray::fromHex("78563412"));
    }

    void qByteArrayWriteU16Be_writesAtOffset()
    {
        QByteArray out = QByteArray::fromHex("aaaaaaaa");
        bytes::writeU16Be(out, 1, 0x1234);
        QCOMPARE(out, QByteArray::fromHex("aa1234aa"));
    }

    void qByteArrayWriteU32Le_writesLittleEndianBytes()
    {
        QByteArray out(4, '\0');
        bytes::writeU32Le(out, 0, 0x12345678);
        QCOMPARE(out, QByteArray::fromHex("78563412"));
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test //tests:test_bytes --test_output=errors`
Expected: FAIL to build — no `bytes::appendU32Be(QByteArray&, ...)` overload exists yet (only the `Bytes&` one from `bytes.h`).

- [ ] **Step 3: Add the overloads to `protocol/qt_bytes.h`**

Insert before the closing `} // namespace bytes` (after `toQByteArray`):

```cpp
inline MutableByteView mutableView(QByteArray& bytes)
{
    return MutableByteView(reinterpret_cast<Byte *>(bytes.data()),
                           static_cast<std::size_t>(bytes.size()));
}

inline void appendU16Be(QByteArray& out, std::uint16_t value)
{
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

inline void appendU24Be(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

inline void appendU32Be(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>((value >> 24) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

inline void appendU16Le(QByteArray& out, std::uint16_t value)
{
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
}

inline void appendU24Le(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
}

inline void appendU32Le(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 24) & 0xFF));
}

inline void writeU16Be(QByteArray& out, std::size_t offset, std::uint16_t value)
{
    writeU16Be(mutableView(out), offset, value);
}

inline void writeU24Be(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    writeU24Be(mutableView(out), offset, value);
}

inline void writeU32Be(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    writeU32Be(mutableView(out), offset, value);
}

inline void writeU16Le(QByteArray& out, std::size_t offset, std::uint16_t value)
{
    writeU16Le(mutableView(out), offset, value);
}

inline void writeU24Le(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    writeU24Le(mutableView(out), offset, value);
}

inline void writeU32Le(QByteArray& out, std::size_t offset, std::uint32_t value)
{
    writeU32Le(mutableView(out), offset, value);
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `bazel test //tests:test_bytes --test_output=errors`
Expected: PASS, all slots including the 4 new ones green.

- [ ] **Step 5: Commit**

```bash
git add protocol/qt_bytes.h tests/test_bytes.cpp
git commit -m "feat: add QByteArray-native append/write overloads to qt_bytes.h"
```

---

### Task 3: Remove dead `EcuOperations` byte/int helpers; fix the one commented-out call site

**Files:**
- Modify: `ecu_operations.h:98-103` (delete declarations)
- Modify: `ecu_operations.cpp:1018-1019`, `ecu_operations.cpp:2217-2251` (delete definitions, fix call site)

**Interfaces:**
- Consumes: `bytes::readU32Be` (existing), `bytes::view` (existing, `protocol/qt_bytes.h`)

- [ ] **Step 1: Confirm `ecu_operations.cpp` already includes `protocol/qt_bytes.h`**

Run: `grep -n "qt_bytes.h" ecu_operations.cpp`
Expected: no output (not yet included) — if so, add `#include "protocol/qt_bytes.h"` near the top of `ecu_operations.cpp`, next to the existing `#include "modules/flash_utils.h"`.

- [ ] **Step 2: Delete the 6 dead declarations from `ecu_operations.h`**

Remove these lines (98-103):

```cpp
int byte_to_int32(unsigned char *data);
int byte_to_int24(unsigned char *data);
int byte_to_int16(unsigned char *data);
void int16_to_byte(unsigned char *data, int i);
void int24_to_byte(unsigned char *data, int i);
void int32_to_byte(unsigned char *data, int i);
```

- [ ] **Step 3: Delete the 6 dead definitions from `ecu_operations.cpp` (lines 2217-2251)**

Remove:

```cpp
int EcuOperations::byte_to_int32(unsigned char *data)
{
    return (data[0] << 24) + (data[1] << 16) + (data[2] << 8) + data[3];
}

int EcuOperations::byte_to_int24(unsigned char *data)
{
    return (data[0] << 16) + (data[1] << 8) + data[2];
}

int EcuOperations::byte_to_int16(unsigned char *data)
{
    return (data[0] << 8) + data[1];
}

void EcuOperations::int16_to_byte(unsigned char *data, int i)
{
    data[0] = i >> 8;
    data[1] = i & 0xFF;
}

void EcuOperations::int24_to_byte(unsigned char *data, int i)
{
    data[0] = i >> 16;
    data[1] = (i >> 8) & 0xFF;
    data[2] = i & 0xFF;
}

void EcuOperations::int32_to_byte(unsigned char *data, int i)
{
    data[0] = i >> 24;
    data[1] = (i >> 16) & 0xFF;
    data[2] = (i >> 8) & 0xFF;
    data[3] = i & 0xFF;
}
```

- [ ] **Step 4: Fix the call site at `ecu_operations.cpp:1018-1019`**

Before:

```cpp
            imgcrc = crc32(src + start, pagesize);
            // romcrc = byte_to_int32(received);
            romcrc = ((received.at(0) & 0xFF) << 24) + ((received.at(1) & 0xFF) << 16) + ((received.at(2) & 0xFF) << 8) + (received.at(3) & 0xFF);
```

After:

```cpp
            imgcrc = crc32(src + start, pagesize);
            romcrc = bytes::readU32Be(bytes::view(received), 0);
```

(`romcrc`'s declared type must remain unchanged — `bytes::readU32Be` returns `std::uint32_t`, which converts implicitly to whatever integer type `romcrc` already is.)

- [ ] **Step 5: Verify it builds**

Run: `bazel build //:fastecu_core_common`
Expected: builds cleanly, no references to the deleted functions remain (confirm with `grep -rn "byte_to_int\|int16_to_byte\|int24_to_byte\|int32_to_byte" --include=*.cpp --include=*.h .` returning nothing).

- [ ] **Step 6: Commit**

```bash
git add ecu_operations.h ecu_operations.cpp
git commit -m "chore: remove dead EcuOperations byte/int helpers, use bytes::readU32Be"
```

---

### Task 4: Convert remaining `ecu_operations.cpp` shift-based read/write call sites

**Files:**
- Modify: `ecu_operations.cpp` (8 call sites: lines ~281-285, ~415-420, ~1072-1082, ~1243-1245, ~1361-1363, ~1506, ~1882-1886, ~1983-1984 — line numbers will have shifted slightly after Task 3's deletions; locate each by the snippet below)

**Interfaces:**
- Consumes: `bytes::writeU16Be(QByteArray&, offset, value)`, `bytes::appendU16Be(QByteArray&, value)`, `bytes::appendU24Be(QByteArray&, value)` (Tasks 1-2)

- [ ] **Step 1: `read_mem_32bit_kline` — two 16-bit BE writes**

Before:

```cpp
        output[2] = numblocks >> 8;
        output[3] = numblocks >> 0;

        output[4] = curblock >> 8;
        output[5] = curblock >> 0;
```

After:

```cpp
        bytes::writeU16Be(output, 2, static_cast<std::uint16_t>(numblocks));
        bytes::writeU16Be(output, 4, static_cast<std::uint16_t>(curblock));
```

- [ ] **Step 2: `read_mem_32bit_can` — two 24-bit BE writes**

Before:

```cpp
        output[6] = (uint8_t)((pagesize >> 24) & 0xFF);
        output[7] = (uint8_t)((pagesize >> 16) & 0xFF);
        output[8] = (uint8_t)((pagesize >> 8) & 0xFF);
        output[9] = (uint8_t)((addr >> 24) & 0xFF);
        output[10] = (uint8_t)((addr >> 16) & 0xFF);
        output[11] = (uint8_t)((addr >> 8) & 0xFF);
```

After:

```cpp
        bytes::writeU24Be(output, 6, pagesize >> 8);
        bytes::writeU24Be(output, 9, addr >> 8);
```

Note: `writeU24Be` writes the bottom 24 bits of whatever value it's given, so reproducing the original's top-24-of-32 truncation (bits 31-8, dropping the low byte) requires pre-shifting `pagesize`/`addr` right by 8 before the call — passing the unshifted 32-bit value would write the wrong 24-bit window (caught in task review: for `pagesize = 0x400`, unshifted gives `[0x00,0x04,0x00]` instead of the original's `[0x00,0x00,0x04]`).

- [ ] **Step 3: `check_romcrc_32bit_kline` — two 16-bit BE appends**

Before:

```cpp
        output.append(chunko >> 8);
        output.append(chunko & 0xFF);
```

After:

```cpp
        bytes::appendU16Be(output, static_cast<std::uint16_t>(chunko));
```

And before:

```cpp
            output.append(chunk_crc >> 8);
            output.append(chunk_crc & 0xFF);
```

After:

```cpp
            bytes::appendU16Be(output, chunk_crc);
```

- [ ] **Step 4: `npk_raw_flashblock_16bit_kline` and `npk_raw_flashblock_32bit_kline` — 24-bit BE appends (2 identical call sites)**

Before (appears twice, once per function):

```cpp
        chksum_data.append(start >> 16);
        chksum_data.append(start >> 8);
        chksum_data.append(start >> 0);
```

After:

```cpp
        bytes::appendU24Be(chksum_data, start);
```

- [ ] **Step 5: `npk_raw_flashblock_32bit_can` — 16-bit BE write**

Before:

```cpp
        output[6] = (uint8_t)((i >> 8) & 0xFF);
        output[7] = (uint8_t)(i & 0xFF);
```

After:

```cpp
        bytes::writeU16Be(output, 6, static_cast<std::uint16_t>(i));
```

- [ ] **Step 6: `reflash_block_32bit_can` — 24-bit + 16-bit BE write**

Before:

```cpp
    output[7] = (uint8_t)((block_start >> 24) & 0xFF);
    output[8] = (uint8_t)((block_start >> 16) & 0xFF);
    output[9] = (uint8_t)((block_start >> 8) & 0xFF);
    output[10] = (uint8_t)((num_128_byte_blocks >> 8) & 0xFF);
    output[11] = (uint8_t)(num_128_byte_blocks & 0xFF);
```

After:

```cpp
    bytes::writeU24Be(output, 7, static_cast<std::uint32_t>(block_start) >> 8);
    bytes::writeU16Be(output, 10, static_cast<std::uint16_t>(num_128_byte_blocks));
```

Same pre-shift note as Step 2: `block_start` is truncated to its top 24 bits (bits 31-8) by the original, so it must be shifted right by 8 before `writeU24Be` extracts the bottom 24 bits of the shifted value. The 16-bit `num_128_byte_blocks` write needs no shift — its 2-line original already matches `writeU16Be`'s natural `>>8`/`&0xFF` decomposition.

- [ ] **Step 7: `read_mem_uj20_30_40_70_kline` — partial 24-bit BE write**

Before:

```cpp
        output[6] = (uint8_t)(addr >> 16) & 0xFF;
        output[7] = (uint8_t)(addr >> 8) & 0xFF;
        output[8] = (uint8_t)addr & 0xFF;
```

After:

```cpp
        bytes::writeU24Be(output, 6, addr);
```

(Original operator precedence was `(uint8_t)(addr >> 16) & 0xFF` — the cast to `uint8_t` already truncates to 8 bits, so the trailing `& 0xFF` was a no-op; `writeU24Be` reproduces the same 3 truncated bytes.)

- [ ] **Step 8: Verify it builds**

Run: `bazel build //:fastecu_core_common`
Expected: builds cleanly.

- [ ] **Step 9: Commit**

```bash
git add ecu_operations.cpp
git commit -m "refactor: ecu_operations.cpp uses bytes:: read/write helpers"
```

---

### Task 5: `modules/ssm_protocol.cpp` — remove local BE reimplementations

**Files:**
- Modify: `modules/ssm_protocol.cpp:12-15,41-47,96-97,119-121`

**Interfaces:**
- Consumes: `bytes::readU32Be`, `bytes::appendU32Be` (existing in `bytes.h`, already `#include`d via `protocol/qt_bytes.h`)

- [ ] **Step 1: Delete the two anonymous-namespace helper functions**

Before (lines 12-15):

```cpp
uint32_t readBigEndianWord(bytes::ByteView data, std::size_t offset)
{
    return ((uint32_t(data[offset]) << 24) & 0xFF000000U) | ((uint32_t(data[offset + 1]) << 16) & 0x00FF0000U) | ((uint32_t(data[offset + 2]) << 8) & 0x0000FF00U) | (uint32_t(data[offset + 3]) & 0x000000FFU);
}
```

Delete this function entirely.

Before (lines 41-47):

```cpp
void appendBigEndianWord(bytes::Bytes *out, uint32_t word)
{
    out->push_back(bytes::Byte(word >> 24));
    out->push_back(bytes::Byte(word >> 16));
    out->push_back(bytes::Byte(word >> 8));
    out->push_back(bytes::Byte(word));
}
```

Delete this function entirely.

- [ ] **Step 2: Update the two call sites**

Before (line 96-97, in `calculateSeedKey`):

```cpp
    appendBigEndianWord(&key, transformWord(readBigEndianWord(seed, 0), keytogenerateindex,
                                            indextransformation, 16, true));
```

After:

```cpp
    bytes::appendU32Be(key, transformWord(bytes::readU32Be(seed, 0), keytogenerateindex,
                                          indextransformation, 16, true));
```

Before (lines 119-121, in `calculatePayload`):

```cpp
        appendBigEndianWord(&encrypted,
                            transformWord(readBigEndianWord(buf, i), keytogenerateindex,
                                          indextransformation, 4, false));
```

After:

```cpp
        bytes::appendU32Be(encrypted,
                          transformWord(bytes::readU32Be(buf, i), keytogenerateindex,
                                        indextransformation, 4, false));
```

- [ ] **Step 3: Run the existing regression test**

Run: `bazel test //tests:test_ssm_protocol --test_output=errors`
Expected: PASS — this suite asserts exact hex-vector outputs of `calculateSeedKey`/`calculatePayload` (e.g. `tests/test_ssm_protocol.cpp:70,83,90,103`), so any behavior change here would fail loudly.

- [ ] **Step 4: Commit**

```bash
git add modules/ssm_protocol.cpp
git commit -m "refactor: ssm_protocol.cpp uses bytes::readU32Be/appendU32Be directly"
```

---

### Task 6: `modules/flash_utils.cpp` — `buildIso15765Request` uses `bytes::appendU32Be`

**Files:**
- Modify: `modules/flash_utils.cpp:41-50`

**Interfaces:**
- Consumes: `bytes::appendU32Be(QByteArray&, value)` (Task 2)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of `modules/flash_utils.cpp`, next to `#include "serial_port_actions.h"`.

- [ ] **Step 2: Replace the manual split**

Before:

```cpp
QByteArray buildIso15765Request(quint32 sourceAddress, const QByteArray& payload)
{
    QByteArray output;
    output.append(char((sourceAddress >> 24) & 0xFF));
    output.append(char((sourceAddress >> 16) & 0xFF));
    output.append(char((sourceAddress >> 8) & 0xFF));
    output.append(char(sourceAddress & 0xFF));
    output.append(payload);
    return output;
}
```

After:

```cpp
QByteArray buildIso15765Request(quint32 sourceAddress, const QByteArray& payload)
{
    QByteArray output;
    bytes::appendU32Be(output, sourceAddress);
    output.append(payload);
    return output;
}
```

- [ ] **Step 3: Run the existing regression test**

Run: `bazel test //tests:test_flash_utils --test_output=errors`
Expected: PASS — `buildIso15765Request_prependsBigEndianSourceAddress` (`tests/test_flash_utils.cpp:35`) asserts exact byte output for both a small and a 29-bit address.

- [ ] **Step 4: Commit**

```bash
git add modules/flash_utils.cpp
git commit -m "refactor: buildIso15765Request uses bytes::appendU32Be"
```

---

### Task 7: `protocol/fastecu_can_transport.cpp` — use `bytes::appendU32Be`/`readU32Be`

**Files:**
- Modify: `protocol/fastecu_can_transport.cpp`

**Interfaces:**
- Consumes: `bytes::appendU32Be(Bytes&, value)`, `bytes::readU32Be(ByteView, offset)` (existing)

- [ ] **Step 1: Replace the write-side split**

Before:

```cpp
int FastEcuCanTransport::write(std::uint32_t canId, bytes::ByteView payload)
{
    bytes::Bytes frame;
    frame.reserve(payload.size() + 4);
    frame.push_back(bytes::Byte(canId >> 24));
    frame.push_back(bytes::Byte(canId >> 16));
    frame.push_back(bytes::Byte(canId >> 8));
    frame.push_back(bytes::Byte(canId));
    frame.insert(frame.end(), payload.begin(), payload.end());
    serial_->write_serial_data_echo_check(bytes::toQByteArray(frame));
    return static_cast<int>(payload.size());
}
```

After:

```cpp
int FastEcuCanTransport::write(std::uint32_t canId, bytes::ByteView payload)
{
    bytes::Bytes frame;
    frame.reserve(payload.size() + 4);
    bytes::appendU32Be(frame, canId);
    frame.insert(frame.end(), payload.begin(), payload.end());
    serial_->write_serial_data_echo_check(bytes::toQByteArray(frame));
    return static_cast<int>(payload.size());
}
```

- [ ] **Step 2: Replace the read-side composition**

Before:

```cpp
    outId = (std::uint32_t(raw[0]) << 24) | (std::uint32_t(raw[1]) << 16) | (std::uint32_t(raw[2]) << 8) | std::uint32_t(raw[3]);
```

After:

```cpp
    outId = bytes::readU32Be(raw, 0);
```

- [ ] **Step 3: Verify it builds**

Run: `bazel build //:fastecu_core_common`
Expected: builds cleanly. (No dedicated test exists for `FastEcuCanTransport` by name; this is a direct, provably-equivalent expression substitution — `readU32Be`/`appendU32Be` implement the identical byte order as the removed manual code.)

- [ ] **Step 4: Commit**

```bash
git add protocol/fastecu_can_transport.cpp
git commit -m "refactor: fastecu_can_transport.cpp uses bytes::appendU32Be/readU32Be"
```

---

### Task 8: `protocol/mitsu_colt_can_cdbg_protocol.cpp` — convert the fixed-width struct-field splits

**Files:**
- Modify: `protocol/mitsu_colt_can_cdbg_protocol.cpp:16-22,94-105,119-143,194-202`

**Interfaces:**
- Consumes: `bytes::writeU32Be(MutableByteView, offset, value)`, `bytes::writeU16Be(MutableByteView, offset, value)` (Task 1) — `CdbgFrame` is `std::array<bytes::Byte, 8>` (`protocol/mitsu_colt_can_cdbg_protocol.h:36`), which converts implicitly to `MutableByteView`.

- [ ] **Step 1: `seedToKey` — replace the initial 4-byte split**

Before:

```cpp
std::uint32_t seedToKey(std::uint32_t seed)
{
    bytes::Byte data[4] = {
        static_cast<bytes::Byte>((seed >> 24) & 0xFF),
        static_cast<bytes::Byte>((seed >> 16) & 0xFF),
        static_cast<bytes::Byte>((seed >> 8) & 0xFF),
        static_cast<bytes::Byte>(seed & 0xFF)};
```

After:

```cpp
std::uint32_t seedToKey(std::uint32_t seed)
{
    bytes::Byte data[4] = {};
    bytes::writeU32Be(data, 0, seed);
```

- [ ] **Step 2: `buildSecurityKeyFrame` — replace the 4-field split**

Before:

```cpp
CdbgFrame buildSecurityKeyFrame(std::uint32_t key)
{
    return CdbgFrame{
        kCmdSecurityKey,
        0,
        static_cast<bytes::Byte>((key >> 24) & 0xFF),
        static_cast<bytes::Byte>((key >> 16) & 0xFF),
        static_cast<bytes::Byte>((key >> 8) & 0xFF),
        static_cast<bytes::Byte>(key & 0xFF),
        0,
        0};
}
```

After:

```cpp
CdbgFrame buildSecurityKeyFrame(std::uint32_t key)
{
    CdbgFrame frame{kCmdSecurityKey, 0, 0, 0, 0, 0, 0, 0};
    bytes::writeU32Be(frame, 2, key);
    return frame;
}
```

- [ ] **Step 3: `buildLogStartFrame` — replace the 2-field split**

Before:

```cpp
    return CdbgFrame{
        kCmdLogStart,
        0,
        1,
        instance,
        frameCount,
        unitFlag,
        static_cast<bytes::Byte>((encoded >> 8) & 0xFF),
        static_cast<bytes::Byte>(encoded & 0xFF)};
```

After:

```cpp
    CdbgFrame frame{kCmdLogStart, 0, 1, instance, frameCount, unitFlag, 0, 0};
    bytes::writeU16Be(frame, 6, encoded);
    return frame;
```

- [ ] **Step 4: `buildFrameInitFrames` — replace the pointer-field split**

Before:

```cpp
        const CdbgChannel& ch = frameItems.at(i);
        out.push_back(CdbgFrame{
            kCmdLogSetPointer,
            0,
            ch.size,
            0,
            static_cast<bytes::Byte>((ch.pointer >> 24) & 0xFF),
            static_cast<bytes::Byte>((ch.pointer >> 16) & 0xFF),
            static_cast<bytes::Byte>((ch.pointer >> 8) & 0xFF),
            static_cast<bytes::Byte>(ch.pointer & 0xFF)});
```

After:

```cpp
        const CdbgChannel& ch = frameItems.at(i);
        CdbgFrame pointerFrame{kCmdLogSetPointer, 0, ch.size, 0, 0, 0, 0, 0};
        bytes::writeU32Be(pointerFrame, 4, ch.pointer);
        out.push_back(pointerFrame);
```

- [ ] **Step 5: Run the existing regression test**

Run: `bazel test //tests:test_mitsu_colt_can_cdbg_protocol --test_output=errors`
Expected: PASS — `tests/test_mitsu_colt_can_cdbg_protocol.cpp` asserts exact `seedToKey` outputs against 6 known vectors (lines 26-31), and exact `buildSecurityKeyFrame`/`buildLogStartFrame`/`buildFrameInitFrames` byte sequences (lines 46, 72, 78, 121).

- [ ] **Step 6: Commit**

```bash
git add protocol/mitsu_colt_can_cdbg_protocol.cpp
git commit -m "refactor: mitsu_colt_can_cdbg_protocol.cpp uses bytes::writeU32Be/writeU16Be"
```

---

### Task 9: `dtc_operations.cpp` — convert the two DTC 16-bit BE reads

**Files:**
- Modify: `dtc_operations.cpp:523,545`

**Interfaces:**
- Consumes: `bytes::readU16Be(ByteView, offset)`, `bytes::view(const QByteArray&)` (existing)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of `dtc_operations.cpp` if not already present (run `grep -n "qt_bytes.h" dtc_operations.cpp` first to check).

- [ ] **Step 2: Convert both call sites**

Before (line 523, in `read_dtc`, stored-DTC loop):

```cpp
        uint16_t dtc = ((uint8_t)response.at(i) << 8) + (uint8_t)response.at(i + 1);
```

After:

```cpp
        uint16_t dtc = bytes::readU16Be(bytes::view(response), static_cast<std::size_t>(i));
```

Before (line 545, in `read_dtc`, pending-DTC loop — identical shape):

```cpp
        uint16_t dtc = ((uint8_t)response.at(i) << 8) + (uint8_t)response.at(i + 1);
```

After:

```cpp
        uint16_t dtc = bytes::readU16Be(bytes::view(response), static_cast<std::size_t>(i));
```

- [ ] **Step 3: Verify it builds**

Run: `bazel build //:fastecu_core_common`
Expected: builds cleanly. (No dedicated `DtcOperations` test exists; this is a direct equivalence substitution over a loop index `i` that is always `>= 0` here, since the loop is `for (int i = 0; i < response.length(); i += 2)`.)

- [ ] **Step 4: Commit**

```bash
git add dtc_operations.cpp
git commit -m "refactor: dtc_operations.cpp uses bytes::readU16Be"
```

---

### Task 10: `get_key_operations_subaru.cpp` — convert the four 32-bit BE reads

**Files:**
- Modify: `get_key_operations_subaru.cpp:110-125,205-220`

**Interfaces:**
- Consumes: `bytes::readU32Be(ByteView, offset)`, `bytes::view(const QByteArray&)` (existing)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of `get_key_operations_subaru.cpp` if not already present.

- [ ] **Step 2: Hoist one pair of views right after the files are loaded**

Both call sites (line ~118 and line ~214) are in the *same* function, `GetKeyOperationsSubaru::load_and_apply_linear_approx()` (`get_key_operations_subaru.cpp:42-321`), so the views are declared once, not per call site.

Before (lines 74-77):

```cpp
    QByteArray encryptedFileData = encryptedFile.readAll();
    encryptedFile.close();

    emit LOG_I("Files loaded successfully", true, true);
```

After:

```cpp
    QByteArray encryptedFileData = encryptedFile.readAll();
    encryptedFile.close();

    const bytes::ByteView unencryptedView = bytes::view(unencryptedFileData);
    const bytes::ByteView encryptedView = bytes::view(encryptedFileData);

    emit LOG_I("Files loaded successfully", true, true);
```

- [ ] **Step 3: Convert the first call site (~line 118-119)**

Before:

```cpp
            total++;
            plainText = ((unencryptedFileData[j] << 24) & 0xFF000000) + ((unencryptedFileData[j + 1] << 16) & 0xFF0000) + ((unencryptedFileData[j + 2] << 8) & 0xFF00) + (unencryptedFileData[j + 3] & 0xFF);
            cipherText = ((encryptedFileData[j] << 24) & 0xFF000000) + ((encryptedFileData[j + 1] << 16) & 0xFF0000) + ((encryptedFileData[j + 2] << 8) & 0xFF00) + (encryptedFileData[j + 3] & 0xFF);
```

After:

```cpp
            total++;
            plainText = bytes::readU32Be(unencryptedView, static_cast<std::size_t>(j));
            cipherText = bytes::readU32Be(encryptedView, static_cast<std::size_t>(j));
```

- [ ] **Step 4: Convert the second call site (~line 214-215), reusing the same views**

Before:

```cpp
            total++;

            plainText = ((unencryptedFileData[j] << 24) & 0xFF000000) + ((unencryptedFileData[j + 1] << 16) & 0xFF0000) + ((unencryptedFileData[j + 2] << 8) & 0xFF00) + (unencryptedFileData[j + 3] & 0xFF);
            cipherText = ((encryptedFileData[j] << 24) & 0xFF000000) + ((encryptedFileData[j + 1] << 16) & 0xFF0000) + ((encryptedFileData[j + 2] << 8) & 0xFF00) + (encryptedFileData[j + 3] & 0xFF);
```

After:

```cpp
            total++;

            plainText = bytes::readU32Be(unencryptedView, static_cast<std::size_t>(j));
            cipherText = bytes::readU32Be(encryptedView, static_cast<std::size_t>(j));
```

- [ ] **Step 5: Verify it builds**

Run: `bazel build //:fastecu_core_common`
Expected: builds cleanly. (No dedicated `GetKeyOperationsSubaru` test exists. `bytes::view` is a zero-copy span over the `QByteArray`'s existing storage — no allocation added to this hot loop, and correctness is a direct algebraic equivalence: the original's `(x[j]<<24)&0xFF000000 + ...` relies on signed-`char`-shift-then-mask "happening to work"; `readU32Be` computes the identical result via well-defined unsigned arithmetic.)

- [ ] **Step 6: Commit**

```bash
git add get_key_operations_subaru.cpp
git commit -m "refactor: get_key_operations_subaru.cpp uses bytes::readU32Be"
```

---

### Task 11: `serial_port/serial_port_actions_direct.cpp` — convert the 16-bit BE read and two 32-bit BE writes

**Files:**
- Modify: `serial_port/serial_port_actions_direct.cpp:820,1441-1444,1466-1473`

**Interfaces:**
- Consumes: `bytes::readU16Be(ByteView, offset)`, `bytes::view`, `bytes::writeU32Be(MutableByteView, offset, value)` (existing/Task 1)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of `serial_port/serial_port_actions_direct.cpp` if not already present.

- [ ] **Step 2: Convert the `read_serial_data` length field read**

Before:

```cpp
                if (received.startsWith("\xbe\xef"))
                    msglen = ((uint8_t)received.at(2) << 8) + (uint8_t)received.at(3) + 1; // +1 for checksum
```

After:

```cpp
                if (received.startsWith("\xbe\xef"))
                    msglen = bytes::readU16Be(bytes::view(received), 2) + 1; // +1 for checksum
```

- [ ] **Step 3: Convert the CAN-only filter write in `set_j2534_can_filters`**

Before:

```cpp
        msgPattern.Data[0] = (can_destination_address >> 24) & 0xFF;
        msgPattern.Data[1] = (can_destination_address >> 16) & 0xFF;
        msgPattern.Data[2] = (can_destination_address >> 8) & 0xFF;
        msgPattern.Data[3] = (can_destination_address & 0xFF);
```

After:

```cpp
        bytes::writeU32Be(msgPattern.Data, 0, can_destination_address);
```

(`msgPattern.Data` is a fixed-size `unsigned char[]` array member of the J2534 `PASSTHRU_MSG` struct — `std::span` deduces its extent automatically when passed to a function expecting `MutableByteView`; confirm with `grep -n "Data\[" <path-to-j2534-header>` if the array-to-span conversion doesn't compile, and pass `bytes::MutableByteView(msgPattern.Data, 4)` explicitly instead.)

- [ ] **Step 4: Convert the ISO15765 filter writes**

Before:

```cpp
        msgPattern.Data[0] = (iso15765_destination_address >> 24) & 0xFF;
        msgPattern.Data[1] = (iso15765_destination_address >> 16) & 0xFF;
        msgPattern.Data[2] = (iso15765_destination_address >> 8) & 0xFF;
        msgPattern.Data[3] = iso15765_destination_address & 0xFF;
        msgFlow.Data[0] = (iso15765_source_address >> 24) & 0xFF;
        msgFlow.Data[1] = (iso15765_source_address >> 16) & 0xFF;
        msgFlow.Data[2] = (iso15765_source_address >> 8) & 0xFF;
        msgFlow.Data[3] = (iso15765_source_address & 0xFF);
```

After:

```cpp
        bytes::writeU32Be(msgPattern.Data, 0, iso15765_destination_address);
        bytes::writeU32Be(msgFlow.Data, 0, iso15765_source_address);
```

(Same array-to-span note as Step 3 — use `bytes::MutableByteView(msgPattern.Data, 4)` / `bytes::MutableByteView(msgFlow.Data, 4)` explicitly if needed.)

- [ ] **Step 5: Verify it builds**

Run: `bazel build //:fastecu_core_common`
Expected: builds cleanly. Cross-check with the existing PTY-backed tests:

Run: `bazel test //tests:test_direct_backend_pty --test_output=errors`
Expected: PASS — `ptyRead_reassemblesFragmentedFrame` and friends exercise `read_serial_data`'s reassembly path end-to-end.

- [ ] **Step 6: Commit**

```bash
git add serial_port/serial_port_actions_direct.cpp
git commit -m "refactor: serial_port_actions_direct.cpp uses bytes:: read/write helpers"
```

---

### Task 12: `modules/biu/biu_operations_subaru.cpp` — convert the three LE 16-bit reads

**Files:**
- Modify: `modules/biu/biu_operations_subaru.cpp:756,785,795`

**Interfaces:**
- Consumes: `bytes::readU16Le(ByteView, offset)`, `bytes::view` (Task 1)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of `modules/biu/biu_operations_subaru.cpp` if not already present.

- [ ] **Step 2: Convert the three call sites**

Before (line 756, front wheel speed):

```cpp
            calc_result = ((uint8_t)message.at(6) << 8) | (uint8_t)message.at(5);
```

After:

```cpp
            calc_result = bytes::readU16Le(bytes::view(message), 5);
```

Before (line 785, fuel level resistance):

```cpp
            calc_result = ((uint8_t)message.at(11) << 8) | (uint8_t)message.at(10);
```

After:

```cpp
            calc_result = bytes::readU16Le(bytes::view(message), 10);
```

Before (line 795, fuel consumption):

```cpp
            calc_result = ((uint8_t)message.at(13) << 8) | (uint8_t)message.at(12);
```

After:

```cpp
            calc_result = bytes::readU16Le(bytes::view(message), 12);
```

- [ ] **Step 3: Verify it builds**

Run: `bazel build //:fastecu_core_common`
Expected: builds cleanly. `calc_result` is a `float` — `bytes::readU16Le` returns `std::uint16_t`, which converts implicitly, matching the original's `uint8_t`-composed-int-to-float conversion.

- [ ] **Step 4: Commit**

```bash
git add modules/biu/biu_operations_subaru.cpp
git commit -m "refactor: biu_operations_subaru.cpp uses bytes::readU16Le"
```

---

### Task 13: `modules/checksum/checksum_ecu_subaru_hitachi_sh72543r.cpp`

**Files:**
- Modify: `modules/checksum/checksum_ecu_subaru_hitachi_sh72543r.cpp:25,36,48-49`

**Interfaces:**
- Consumes: `bytes::readU16Be(ByteView, offset)`, `bytes::appendU16Be(QByteArray&, value)`, `bytes::view` (existing/Task 2)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of the file.

- [ ] **Step 2: Convert the summation loop read**

Before:

```cpp
        chksum += ((uint8_t)romData.at(i) << 8) + (uint8_t)romData.at(i + 1);
```

After:

```cpp
        chksum += bytes::readU16Be(bytes::view(romData), static_cast<std::size_t>(i));
```

- [ ] **Step 3: Convert the balance-value read**

Before:

```cpp
        uint16_t balance_value = ((uint8_t)romData.at(balance_value_array_start) << 8) + ((uint8_t)romData.at(balance_value_array_start + 1));
```

After:

```cpp
        uint16_t balance_value = bytes::readU16Be(bytes::view(romData), balance_value_array_start);
```

- [ ] **Step 4: Convert the balance-value write**

Before:

```cpp
        balance_value_array.append((uint8_t)((balance_value >> 8) & 0xff));
        balance_value_array.append((uint8_t)(balance_value & 0xff));
```

After:

```cpp
        bytes::appendU16Be(balance_value_array, balance_value);
```

- [ ] **Step 5: Run the existing regression test**

Run: `bazel test //tests:test_checksum_results --test_output=errors`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add modules/checksum/checksum_ecu_subaru_hitachi_sh72543r.cpp
git commit -m "refactor: checksum_ecu_subaru_hitachi_sh72543r.cpp uses bytes:: helpers"
```

---

### Task 14: `modules/checksum/checksum_tcu_subaru_denso_sh7055.cpp`

**Files:**
- Modify: `modules/checksum/checksum_tcu_subaru_denso_sh7055.cpp:50,65,77-78`

**Interfaces:**
- Consumes: `bytes::readU16Be`, `bytes::appendU16Be`, `bytes::view` (existing/Task 2)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of the file.

- [ ] **Step 2: Convert the area-summation loop read**

Before:

```cpp
            checksum += ((uint8_t)romData.at(j) << 8) + ((uint8_t)romData.at(j + 1));
```

After:

```cpp
            checksum += bytes::readU16Be(bytes::view(romData), j);
```

- [ ] **Step 3: Convert the balance-value read**

Before:

```cpp
        uint16_t balance_value = ((uint8_t)romData.at(0x7fff4) << 8) + ((uint8_t)romData.at(0x7fff5));
```

After:

```cpp
        uint16_t balance_value = bytes::readU16Be(bytes::view(romData), 0x7fff4);
```

- [ ] **Step 4: Convert the balance-value write**

Before:

```cpp
        balance_value_array.append((uint8_t)((balance_value >> 8) & 0xff));
        balance_value_array.append((uint8_t)(balance_value & 0xff));
```

After:

```cpp
        bytes::appendU16Be(balance_value_array, balance_value);
```

- [ ] **Step 5: Run the existing regression test**

Run: `bazel test //tests:test_checksum_results --test_output=errors`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add modules/checksum/checksum_tcu_subaru_denso_sh7055.cpp
git commit -m "refactor: checksum_tcu_subaru_denso_sh7055.cpp uses bytes:: helpers"
```

---

### Task 15: `modules/checksum/checksum_tcu_mitsu_mh8104_can.cpp`

**Files:**
- Modify: `modules/checksum/checksum_tcu_mitsu_mh8104_can.cpp:33,51-54,72-75`

**Interfaces:**
- Consumes: `bytes::readU32Be`, `bytes::appendU32Be`, `bytes::view` (existing/Task 2)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of the file.

- [ ] **Step 2: Convert the summation loop read**

Before:

```cpp
        checksum += ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
```

After:

```cpp
        checksum += bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
```

- [ ] **Step 3: Convert the balance-value read**

Before:

```cpp
        checksum_balance_value = ((uint8_t)romData.at(checksum_balance_value_address) << 24);
        checksum_balance_value += ((uint8_t)romData.at(checksum_balance_value_address + 1) << 16);
        checksum_balance_value += ((uint8_t)romData.at(checksum_balance_value_address + 2) << 8);
        checksum_balance_value += ((uint8_t)romData.at(checksum_balance_value_address + 3));
```

After:

```cpp
        checksum_balance_value = bytes::readU32Be(bytes::view(romData), checksum_balance_value_address);
```

- [ ] **Step 4: Convert the balance-value write**

Before:

```cpp
        balance_value_array.append((uint8_t)((checksum_balance_value >> 24) & 0xff));
        balance_value_array.append((uint8_t)((checksum_balance_value >> 16) & 0xff));
        balance_value_array.append((uint8_t)((checksum_balance_value >> 8) & 0xff));
        balance_value_array.append((uint8_t)(checksum_balance_value & 0xff));
```

After:

```cpp
        bytes::appendU32Be(balance_value_array, checksum_balance_value);
```

- [ ] **Step 5: Run the existing regression test**

Run: `bazel test //tests:test_checksum_results --test_output=errors`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add modules/checksum/checksum_tcu_mitsu_mh8104_can.cpp
git commit -m "refactor: checksum_tcu_mitsu_mh8104_can.cpp uses bytes:: helpers"
```

---

### Task 16: `modules/checksum/checksum_ecu_subaru_hitachi_m32r_kline.cpp`

**Files:**
- Modify: `modules/checksum/checksum_ecu_subaru_hitachi_m32r_kline.cpp:89,91,98,110-111`

**Interfaces:**
- Consumes: `bytes::readU32Be`, `bytes::readU16Be`, `bytes::appendU16Be`, `bytes::view` (existing/Task 2)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of the file.

- [ ] **Step 2: Convert the checksum-3 summation loop read**

Before:

```cpp
            checksum_3_value_calculated += ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
```

After:

```cpp
            checksum_3_value_calculated += bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
```

- [ ] **Step 3: Convert the two balance-value reads**

Before:

```cpp
    checksum_3_balance_value_stored = ((uint8_t)romData.at(checksum_3_balance_value_address) << 8) + (uint8_t)romData.at(checksum_3_balance_value_address + 1);
```

After:

```cpp
    checksum_3_balance_value_stored = bytes::readU16Be(bytes::view(romData), checksum_3_balance_value_address);
```

Before:

```cpp
        uint16_t balance_value = (uint16_t)(romData.at(checksum_3_balance_value_address) << 8) + (uint16_t)(romData.at(checksum_3_balance_value_address + 1));
```

After:

```cpp
        uint16_t balance_value = bytes::readU16Be(bytes::view(romData), checksum_3_balance_value_address);
```

(This also fixes the latent signed-`char`-shift issue in the original — `romData.at(...)` returns `char`, so `romData.at(addr) << 8` was shifting a possibly-negative value before the `uint16_t` cast; `readU16Be` computes the same intended value via well-defined unsigned arithmetic.)

- [ ] **Step 4: Convert the balance-value write**

Before:

```cpp
        balance_value_array.append((uint8_t)((balance_value >> 8) & 0xff));
        balance_value_array.append((uint8_t)(balance_value & 0xff));
```

After:

```cpp
        bytes::appendU16Be(balance_value_array, balance_value);
```

- [ ] **Step 5: Run the existing regression test**

Run: `bazel test //tests:test_checksum_results --test_output=errors`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add modules/checksum/checksum_ecu_subaru_hitachi_m32r_kline.cpp
git commit -m "refactor: checksum_ecu_subaru_hitachi_m32r_kline.cpp uses bytes:: helpers"
```

---

### Task 17: `modules/checksum/checksum_ecu_subaru_hitachi_m32r_can.cpp`

**Files:**
- Modify: `modules/checksum/checksum_ecu_subaru_hitachi_m32r_can.cpp:57-61,68-71,86-89,107-108,111-112,119-122,136-140,157,159,167,179-180,198-199,203,210-211`

**Interfaces:**
- Consumes: `bytes::readU32Be`, `bytes::readU16Be`, `bytes::appendU32Be`, `bytes::appendU16Be`, `bytes::view` (existing/Task 2)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of the file.

- [ ] **Step 2: Convert the checksum 1/2 summation loop (lines 57-58)**

Before:

```cpp
        checksum_1_value_calculated += ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
        checksum_2_value_calculated ^= ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
```

After:

```cpp
        const std::uint32_t word = bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
        checksum_1_value_calculated += word;
        checksum_2_value_calculated ^= word;
```

- [ ] **Step 3: Convert the checksum 1/2 stored reads (lines 60-61)**

Before:

```cpp
    checksum_1_value_stored = ((uint8_t)romData.at(checksum_1_value_address) << 24) + ((uint8_t)romData.at(checksum_1_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_1_value_address + 2) << 8) + (uint8_t)romData.at(checksum_1_value_address + 3);
    checksum_2_value_stored = ((uint8_t)romData.at(checksum_2_value_address) << 24) + ((uint8_t)romData.at(checksum_2_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_2_value_address + 2) << 8) + (uint8_t)romData.at(checksum_2_value_address + 3);
```

After:

```cpp
    checksum_1_value_stored = bytes::readU32Be(bytes::view(romData), checksum_1_value_address);
    checksum_2_value_stored = bytes::readU32Be(bytes::view(romData), checksum_2_value_address);
```

- [ ] **Step 4: Convert the checksum 1/2 correction writes (lines 68-71, 86-89)**

Before (checksum 1):

```cpp
        checksum.append((uint8_t)(checksum_1_value_calculated >> 24));
        checksum.append((uint8_t)(checksum_1_value_calculated >> 16));
        checksum.append((uint8_t)(checksum_1_value_calculated >> 8));
        checksum.append((uint8_t)checksum_1_value_calculated);
```

After:

```cpp
        bytes::appendU32Be(checksum, checksum_1_value_calculated);
```

Before (checksum 2, identical shape):

```cpp
        checksum.append((uint8_t)(checksum_2_value_calculated >> 24));
        checksum.append((uint8_t)(checksum_2_value_calculated >> 16));
        checksum.append((uint8_t)(checksum_2_value_calculated >> 8));
        checksum.append((uint8_t)checksum_2_value_calculated);
```

After:

```cpp
        bytes::appendU32Be(checksum, checksum_2_value_calculated);
```

- [ ] **Step 5: Convert the checksum 3/4 summation loop (lines 107-108)**

Before:

```cpp
            checksum_3_value_calculated += ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
            checksum_4_value_calculated ^= ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
```

After:

```cpp
            const std::uint32_t word2 = bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
            checksum_3_value_calculated += word2;
            checksum_4_value_calculated ^= word2;
```

- [ ] **Step 6: Convert the checksum 3/4 stored reads (lines 111-112) and correction writes (lines 119-122, 136-140)**

Before:

```cpp
    checksum_3_value_stored = ((uint8_t)romData.at(checksum_3_value_address) << 24) + ((uint8_t)romData.at(checksum_3_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_3_value_address + 2) << 8) + (uint8_t)romData.at(checksum_3_value_address + 3);
    checksum_4_value_stored = ((uint8_t)romData.at(checksum_4_value_address) << 24) + ((uint8_t)romData.at(checksum_4_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_4_value_address + 2) << 8) + (uint8_t)romData.at(checksum_4_value_address + 3);
```

After:

```cpp
    checksum_3_value_stored = bytes::readU32Be(bytes::view(romData), checksum_3_value_address);
    checksum_4_value_stored = bytes::readU32Be(bytes::view(romData), checksum_4_value_address);
```

Before (checksum 3 write, lines 119-122):

```cpp
        checksum.append((uint8_t)(checksum_3_value_calculated >> 24));
        checksum.append((uint8_t)(checksum_3_value_calculated >> 16));
        checksum.append((uint8_t)(checksum_3_value_calculated >> 8));
        checksum.append((uint8_t)checksum_3_value_calculated);
```

After:

```cpp
        bytes::appendU32Be(checksum, checksum_3_value_calculated);
```

Before (checksum 4 write, lines 136-140):

```cpp
        checksum.append((uint8_t)(checksum_4_value_calculated >> 24));
        checksum.append((uint8_t)(checksum_4_value_calculated >> 16));
        checksum.append((uint8_t)(checksum_4_value_calculated >> 8));
        checksum.append((uint8_t)checksum_4_value_calculated);
```

After:

```cpp
        bytes::appendU32Be(checksum, checksum_4_value_calculated);
```

- [ ] **Step 7: Convert checksum 5's reads and write (lines 157, 159, 167, 179-180)**

Before:

```cpp
            checksum_5_value_calculated += ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
```

After:

```cpp
            checksum_5_value_calculated += bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
```

Before:

```cpp
    checksum_5_balance_value_stored = ((uint8_t)romData.at(checksum_5_balance_value_address) << 8) + (uint8_t)romData.at(checksum_5_balance_value_address + 1);
```

After:

```cpp
    checksum_5_balance_value_stored = bytes::readU16Be(bytes::view(romData), checksum_5_balance_value_address);
```

Before:

```cpp
        uint16_t balance_value = (uint16_t)(romData.at(checksum_5_balance_value_address) << 8) + (uint16_t)(romData.at(checksum_5_balance_value_address + 1));
```

After:

```cpp
        uint16_t balance_value = bytes::readU16Be(bytes::view(romData), checksum_5_balance_value_address);
```

Before:

```cpp
        balance_value_array.append((uint8_t)((balance_value >> 8) & 0xff));
        balance_value_array.append((uint8_t)(balance_value & 0xff));
```

After:

```cpp
        bytes::appendU16Be(balance_value_array, balance_value);
```

- [ ] **Step 8: Convert checksum 6's stored read and write (lines 203, 210-211)**

Before:

```cpp
    checksum_6_value_stored = ((uint8_t)romData.at(checksum_6_value_address) << 8) + (uint8_t)romData.at(checksum_6_value_address + 1);
```

After:

```cpp
    checksum_6_value_stored = bytes::readU16Be(bytes::view(romData), checksum_6_value_address);
```

Before:

```cpp
        checksum.append((uint8_t)(checksum_6_value_calculated >> 8));
        checksum.append((uint8_t)checksum_6_value_calculated);
```

After:

```cpp
        bytes::appendU16Be(checksum, checksum_6_value_calculated);
```

- [ ] **Step 9: Run the existing regression test**

Run: `bazel test //tests:test_checksum_results --test_output=errors`
Expected: PASS.

- [ ] **Step 10: Commit**

```bash
git add modules/checksum/checksum_ecu_subaru_hitachi_m32r_can.cpp
git commit -m "refactor: checksum_ecu_subaru_hitachi_m32r_can.cpp uses bytes:: helpers"
```

---

### Task 18: `modules/checksum/checksum_ecu_subaru_hitachi_sh7058.cpp`

**Files:**
- Modify: `modules/checksum/checksum_ecu_subaru_hitachi_sh7058.cpp:50-54,61-64,79-83,99,104,116-120,131,135-136,143-147,191,196-200`

**Interfaces:**
- Consumes: `bytes::readU32Be`, `bytes::appendU32Be`, `bytes::view` (existing/Task 2)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of the file.

- [ ] **Step 2: Convert the checksum 1/2 summation loop (lines 50-51)**

Before:

```cpp
        checksum_1_value_calculated += ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
        checksum_2_value_calculated ^= ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
```

After:

```cpp
        const std::uint32_t word = bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
        checksum_1_value_calculated += word;
        checksum_2_value_calculated ^= word;
```

- [ ] **Step 3: Convert the checksum 1/2 stored reads (lines 53-54) and writes (lines 61-64, 79-83)**

Before:

```cpp
    checksum_1_value_stored = ((uint8_t)romData.at(checksum_1_value_address) << 24) + ((uint8_t)romData.at(checksum_1_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_1_value_address + 2) << 8) + (uint8_t)romData.at(checksum_1_value_address + 3);
    checksum_2_value_stored = ((uint8_t)romData.at(checksum_2_value_address) << 24) + ((uint8_t)romData.at(checksum_2_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_2_value_address + 2) << 8) + (uint8_t)romData.at(checksum_2_value_address + 3);
```

After:

```cpp
    checksum_1_value_stored = bytes::readU32Be(bytes::view(romData), checksum_1_value_address);
    checksum_2_value_stored = bytes::readU32Be(bytes::view(romData), checksum_2_value_address);
```

Before (checksum 1 write, lines 61-64):

```cpp
        checksum.append((uint8_t)(checksum_1_value_calculated >> 24));
        checksum.append((uint8_t)(checksum_1_value_calculated >> 16));
        checksum.append((uint8_t)(checksum_1_value_calculated >> 8));
        checksum.append((uint8_t)checksum_1_value_calculated);
```

After:

```cpp
        bytes::appendU32Be(checksum, checksum_1_value_calculated);
```

Before (checksum 2 write, lines 79-83, identical shape):

```cpp
        checksum.append((uint8_t)(checksum_2_value_calculated >> 24));
        checksum.append((uint8_t)(checksum_2_value_calculated >> 16));
        checksum.append((uint8_t)(checksum_2_value_calculated >> 8));
        checksum.append((uint8_t)checksum_2_value_calculated);
```

After:

```cpp
        bytes::appendU32Be(checksum, checksum_2_value_calculated);
```

- [ ] **Step 4: Convert checksum 5's summation loop read (line 99) and balance-value read/write (lines 104, 116-120)**

Before:

```cpp
            checksum_5_value_calculated += ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
```

After:

```cpp
            checksum_5_value_calculated += bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
```

Before:

```cpp
        uint32_t balance_value = ((uint8_t)romData.at(checksum_5_balance_value_address) << 24) + ((uint8_t)romData.at(checksum_5_balance_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_5_balance_value_address + 2) << 8) + ((uint8_t)romData.at(checksum_5_balance_value_address + 3));
```

After:

```cpp
        uint32_t balance_value = bytes::readU32Be(bytes::view(romData), checksum_5_balance_value_address);
```

Before:

```cpp
        balance_value_array.append((uint8_t)((balance_value >> 24) & 0xff));
        balance_value_array.append((uint8_t)((balance_value >> 16) & 0xff));
        balance_value_array.append((uint8_t)((balance_value >> 8) & 0xff));
        balance_value_array.append((uint8_t)(balance_value & 0xff));
```

After:

```cpp
        bytes::appendU32Be(balance_value_array, balance_value);
```

- [ ] **Step 5: Convert checksum 3/4's summation loop (line 131), stored reads (lines 135-136), and writes (lines 143-147, 196-200)**

Before:

```cpp
            checksum_3_value_calculated += ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
            checksum_4_value_calculated ^= ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
```

After:

```cpp
            const std::uint32_t word2 = bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
            checksum_3_value_calculated += word2;
            checksum_4_value_calculated ^= word2;
```

Before:

```cpp
    checksum_3_value_stored = ((uint8_t)romData.at(checksum_3_value_address) << 24) + ((uint8_t)romData.at(checksum_3_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_3_value_address + 2) << 8) + (uint8_t)romData.at(checksum_3_value_address + 3);
    checksum_4_value_stored = ((uint8_t)romData.at(checksum_4_value_address) << 24) + ((uint8_t)romData.at(checksum_4_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_4_value_address + 2) << 8) + (uint8_t)romData.at(checksum_4_value_address + 3);
```

After:

```cpp
    checksum_3_value_stored = bytes::readU32Be(bytes::view(romData), checksum_3_value_address);
    checksum_4_value_stored = bytes::readU32Be(bytes::view(romData), checksum_4_value_address);
```

Before (checksum 3 write, lines 143-147, inside a block that also has a commented-out alternate implementation — leave the comment block untouched):

```cpp
        checksum.append((uint8_t)(checksum_3_value_calculated >> 24));
        checksum.append((uint8_t)(checksum_3_value_calculated >> 16));
        checksum.append((uint8_t)(checksum_3_value_calculated >> 8));
        checksum.append((uint8_t)checksum_3_value_calculated);
        romData.replace(checksum_3_value_address, checksum.length(), checksum);
```

After:

```cpp
        bytes::appendU32Be(checksum, checksum_3_value_calculated);
        romData.replace(checksum_3_value_address, checksum.length(), checksum);
```

Before (checksum 4's second read-loop-then-write, lines 187-200 — the recompute block inside the `if` on line 177):

```cpp
        checksum_4_value_calculated = 0;
        for (uint32_t i = 0x0000; i < 0x100000; i += 4)
        {
            if (i != checksum_3_value_address && i != checksum_4_value_address)
            {
                checksum_4_value_calculated ^= ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
            }
        }

        QByteArray checksum;
        checksum.append((uint8_t)(checksum_4_value_calculated >> 24));
        checksum.append((uint8_t)(checksum_4_value_calculated >> 16));
        checksum.append((uint8_t)(checksum_4_value_calculated >> 8));
        checksum.append((uint8_t)checksum_4_value_calculated);
        romData.replace(checksum_4_value_address, checksum.length(), checksum);
```

After:

```cpp
        checksum_4_value_calculated = 0;
        for (uint32_t i = 0x0000; i < 0x100000; i += 4)
        {
            if (i != checksum_3_value_address && i != checksum_4_value_address)
            {
                checksum_4_value_calculated ^= bytes::readU32Be(bytes::view(romData), i);
            }
        }

        QByteArray checksum;
        bytes::appendU32Be(checksum, checksum_4_value_calculated);
        romData.replace(checksum_4_value_address, checksum.length(), checksum);
```

- [ ] **Step 6: Run the existing regression test**

Run: `bazel test //tests:test_checksum_results --test_output=errors`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add modules/checksum/checksum_ecu_subaru_hitachi_sh7058.cpp
git commit -m "refactor: checksum_ecu_subaru_hitachi_sh7058.cpp uses bytes:: helpers"
```

---

### Task 19: `modules/checksum/checksum_tcu_subaru_hitachi_m32r_can.cpp`

**Files:**
- Modify: `modules/checksum/checksum_tcu_subaru_hitachi_m32r_can.cpp:27,29,39,48,70-73,97,108,111`

**Interfaces:**
- Consumes: `bytes::readU32Be`, `bytes::appendU32Be`, `bytes::view` (existing/Task 2)

**Note:** lines 32-37 and 99-104 (the `checksum_2_value_calculated_bytes[4]` scramble that computes `0xff - byte` / `0x100 - byte` per position before recomposing) are algorithm-internal — this isn't a plain byte-array-to-int read, it's a per-byte one's-complement transform. Leave those two blocks untouched; only convert the plain reads/writes below.

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of the file.

- [ ] **Step 2: Convert the two summation-loop reads (lines 27, 29)**

Before:

```cpp
        if (i >= 0x8020)
            checksum_1_value_calculated += ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
        if (i < 0x8000 || i > 0x8007)
            checksum_2_value_calculated += ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
```

After:

```cpp
        if (i >= 0x8020)
            checksum_1_value_calculated += bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
        if (i < 0x8000 || i > 0x8007)
            checksum_2_value_calculated += bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
```

- [ ] **Step 3: Convert the two stored-value reads (lines 39, 48)**

Before:

```cpp
    checksum_1_balance_value_stored = ((uint8_t)romData.at(checksum_1_balance_value_address) << 24) + ((uint8_t)romData.at(checksum_1_balance_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_1_balance_value_address + 2) << 8) + (uint8_t)romData.at(checksum_1_balance_value_address + 3);
```

After:

```cpp
    checksum_1_balance_value_stored = bytes::readU32Be(bytes::view(romData), checksum_1_balance_value_address);
```

Before:

```cpp
    checksum_2_value_stored = ((uint8_t)romData.at(checksum_2_balance_value_address) << 24) + ((uint8_t)romData.at(checksum_2_balance_value_address + 1) << 16) + ((uint8_t)romData.at(checksum_2_balance_value_address + 2) << 8) + (uint8_t)romData.at(checksum_2_balance_value_address + 3);
```

After:

```cpp
    checksum_2_value_stored = bytes::readU32Be(bytes::view(romData), checksum_2_balance_value_address);
```

- [ ] **Step 4: Convert the checksum-1 correction write (lines 70-73)**

Before:

```cpp
        checksum_value_array.append((uint8_t)((checksum_1_balance_value_stored >> 24) & 0xff));
        checksum_value_array.append((uint8_t)((checksum_1_balance_value_stored >> 16) & 0xff));
        checksum_value_array.append((uint8_t)((checksum_1_balance_value_stored >> 8) & 0xff));
        checksum_value_array.append((uint8_t)(checksum_1_balance_value_stored & 0xff));
```

After:

```cpp
        bytes::appendU32Be(checksum_value_array, checksum_1_balance_value_stored);
```

- [ ] **Step 5: Convert the checksum-2 recompute loop read (line 97)**

Before:

```cpp
            if (i < 0x8000 || i > 0x8007)
                checksum_2_value_calculated += ((uint8_t)romData.at(i) << 24) + ((uint8_t)romData.at(i + 1) << 16) + ((uint8_t)romData.at(i + 2) << 8) + (uint8_t)romData.at(i + 3);
```

After:

```cpp
            if (i < 0x8000 || i > 0x8007)
                checksum_2_value_calculated += bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
```

- [ ] **Step 6: Leave lines 108-111 untouched**

Before/unchanged:

```cpp
        for (int i = 0; i < 2; i++)
        {
            balance_value_array.append((uint8_t)((checksum_2_value_calculated >> 24) & 0xff));
            balance_value_array.append((uint8_t)((checksum_2_value_calculated >> 16) & 0xff));
            balance_value_array.append((uint8_t)((checksum_2_value_calculated >> 8) & 0xff));
            balance_value_array.append((uint8_t)(checksum_2_value_calculated & 0xff));
        }
```

This one is a candidate for `bytes::appendU32Be` too — convert it identically to Step 4's pattern:

```cpp
        for (int i = 0; i < 2; i++)
        {
            bytes::appendU32Be(balance_value_array, checksum_2_value_calculated);
        }
```

- [ ] **Step 7: Run the existing regression test**

Run: `bazel test //tests:test_checksum_results --test_output=errors`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add modules/checksum/checksum_tcu_subaru_hitachi_m32r_can.cpp
git commit -m "refactor: checksum_tcu_subaru_hitachi_m32r_can.cpp uses bytes:: helpers"
```

---

### Task 20: `modules/checksum/checksum_ecu_subaru_denso_sh705x_diesel.cpp`

**Files:**
- Modify: `modules/checksum/checksum_ecu_subaru_denso_sh705x_diesel.cpp:37-39,58,80-91,120-122,131,148-159`

**Interfaces:**
- Consumes: `bytes::readU32Be`, `bytes::appendU32Be`, `bytes::view` (existing/Task 2)

**Note:** the `checksum_dword_addr_lo`/`checksum_dword_addr_hi`/`checksum_diff` triple-read (lines 37-39, and its duplicate at 120-122) walks 4 bytes with a running `(x << 8) + byte` accumulate *starting fresh each inner loop* — this is exactly `readU32Be` at the loop's starting offset `i`/`i+4`/`i+8`, since each inner `for (j...)` loop's 4 iterations reconstruct one 32-bit BE word from 4 consecutive bytes.

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of the file.

- [ ] **Step 2: Convert the first triple-word read (lines 35-40)**

Before:

```cpp
        for (int j = 0; j < 4; j++)
        {
            checksum_dword_addr_lo = (checksum_dword_addr_lo << 8) + (uint8_t)romData.at(i + j);
            checksum_dword_addr_hi = (checksum_dword_addr_hi << 8) + (uint8_t)romData.at(i + 4 + j);
            checksum_diff = (checksum_diff << 8) + (uint8_t)romData.at(i + 8 + j);
        }
```

After:

```cpp
        checksum_dword_addr_lo = bytes::readU32Be(bytes::view(romData), i);
        checksum_dword_addr_hi = bytes::readU32Be(bytes::view(romData), i + 4);
        checksum_diff = bytes::readU32Be(bytes::view(romData), i + 8);
```

- [ ] **Step 3: Convert the inner-block checksum_temp read (lines 56-59)**

Before:

```cpp
                for (int k = 0; k < 4; k++)
                {
                    checksum_temp = (checksum_temp << 8) + (uint8_t)(romData.at(j + k));
                }
```

After:

```cpp
                checksum_temp = bytes::readU32Be(bytes::view(romData), j);
```

- [ ] **Step 4: Convert the 12-byte block append (lines 80-91)**

Before:

```cpp
        checksum_array.append(checksum_dword_addr_lo >> 24);
        checksum_array.append(checksum_dword_addr_lo >> 16);
        checksum_array.append(checksum_dword_addr_lo >> 8);
        checksum_array.append(checksum_dword_addr_lo);
        checksum_array.append(checksum_dword_addr_hi >> 24);
        checksum_array.append(checksum_dword_addr_hi >> 16);
        checksum_array.append(checksum_dword_addr_hi >> 8);
        checksum_array.append(checksum_dword_addr_hi);
        checksum_array.append(checksum_check >> 24);
        checksum_array.append(checksum_check >> 16);
        checksum_array.append(checksum_check >> 8);
        checksum_array.append(checksum_check);
```

After:

```cpp
        bytes::appendU32Be(checksum_array, checksum_dword_addr_lo);
        bytes::appendU32Be(checksum_array, checksum_dword_addr_hi);
        bytes::appendU32Be(checksum_array, checksum_check);
```

- [ ] **Step 5: Repeat Steps 2-4 for the SH72543 EURO6 additional-checksum block (lines 118-159, identical shapes)**

Before (triple-word read, lines 118-123):

```cpp
            for (int j = 0; j < 4; j++)
            {
                checksum_dword_addr_lo = (checksum_dword_addr_lo << 8) + (uint8_t)romData.at(i + j);
                checksum_dword_addr_hi = (checksum_dword_addr_hi << 8) + (uint8_t)romData.at(i + 4 + j);
                checksum_diff = (checksum_diff << 8) + (uint8_t)romData.at(i + 8 + j);
            }
```

After:

```cpp
            checksum_dword_addr_lo = bytes::readU32Be(bytes::view(romData), i);
            checksum_dword_addr_hi = bytes::readU32Be(bytes::view(romData), i + 4);
            checksum_diff = bytes::readU32Be(bytes::view(romData), i + 8);
```

Before (inner checksum_temp read, lines 129-132):

```cpp
                    for (int k = 0; k < 4; k++)
                    {
                        checksum_temp = (checksum_temp << 8) + (uint8_t)(romData.at(j + k));
                    }
```

After:

```cpp
                    checksum_temp = bytes::readU32Be(bytes::view(romData), j);
```

Before (12-byte append, lines 148-159):

```cpp
            checksum_array.append(checksum_dword_addr_lo >> 24);
            checksum_array.append(checksum_dword_addr_lo >> 16);
            checksum_array.append(checksum_dword_addr_lo >> 8);
            checksum_array.append(checksum_dword_addr_lo);
            checksum_array.append(checksum_dword_addr_hi >> 24);
            checksum_array.append(checksum_dword_addr_hi >> 16);
            checksum_array.append(checksum_dword_addr_hi >> 8);
            checksum_array.append(checksum_dword_addr_hi);
            checksum_array.append(checksum_check >> 24);
            checksum_array.append(checksum_check >> 16);
            checksum_array.append(checksum_check >> 8);
            checksum_array.append(checksum_check);
```

After:

```cpp
            bytes::appendU32Be(checksum_array, checksum_dword_addr_lo);
            bytes::appendU32Be(checksum_array, checksum_dword_addr_hi);
            bytes::appendU32Be(checksum_array, checksum_check);
```

- [ ] **Step 6: Run the existing regression test**

Run: `bazel test //tests:test_checksum_results --test_output=errors`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add modules/checksum/checksum_ecu_subaru_denso_sh705x_diesel.cpp
git commit -m "refactor: checksum_ecu_subaru_denso_sh705x_diesel.cpp uses bytes:: helpers"
```

---

### Task 21: `modules/checksum/checksum_ecu_subaru_denso_sh7xxx.cpp`

**Files:**
- Modify: `modules/checksum/checksum_ecu_subaru_denso_sh7xxx.cpp:56-61,90-93,109-120`

**Interfaces:**
- Consumes: `bytes::readU32Be`, `bytes::appendU32Be`, `bytes::view` (existing/Task 2)

- [ ] **Step 1: Add the include**

Add `#include "protocol/qt_bytes.h"` near the top of the file.

- [ ] **Step 2: Convert the triple-word read (lines 56-61)**

Before:

```cpp
        for (int j = 0; j < 4; j++)
        {
            checksum_dword_addr_lo = (checksum_dword_addr_lo << 8) + (uint8_t)romData.at(i + j);
            checksum_dword_addr_hi = (checksum_dword_addr_hi << 8) + (uint8_t)romData.at(i + 4 + j);
            checksum_diff = (checksum_diff << 8) + (uint8_t)romData.at(i + 8 + j);
        }
```

After:

```cpp
        checksum_dword_addr_lo = bytes::readU32Be(bytes::view(romData), i);
        checksum_dword_addr_hi = bytes::readU32Be(bytes::view(romData), i + 4);
        checksum_diff = bytes::readU32Be(bytes::view(romData), i + 8);
```

- [ ] **Step 3: Convert the inner checksum_temp read (lines 90-93)**

Before:

```cpp
                for (int k = 0; k < 4; k++)
                {
                    checksum_temp = (checksum_temp << 8) + (uint8_t)(romData.at(j + k));
                }
```

After:

```cpp
                checksum_temp = bytes::readU32Be(bytes::view(romData), j);
```

- [ ] **Step 4: Convert the 12-byte block append (lines 109-120)**

Before:

```cpp
        checksum_array.append(checksum_dword_addr_lo >> 24);
        checksum_array.append(checksum_dword_addr_lo >> 16);
        checksum_array.append(checksum_dword_addr_lo >> 8);
        checksum_array.append(checksum_dword_addr_lo);
        checksum_array.append(checksum_dword_addr_hi >> 24);
        checksum_array.append(checksum_dword_addr_hi >> 16);
        checksum_array.append(checksum_dword_addr_hi >> 8);
        checksum_array.append(checksum_dword_addr_hi);
        checksum_array.append(checksum_check >> 24);
        checksum_array.append(checksum_check >> 16);
        checksum_array.append(checksum_check >> 8);
        checksum_array.append(checksum_check);
```

After:

```cpp
        bytes::appendU32Be(checksum_array, checksum_dword_addr_lo);
        bytes::appendU32Be(checksum_array, checksum_dword_addr_hi);
        bytes::appendU32Be(checksum_array, checksum_check);
```

- [ ] **Step 5: Run the existing regression test**

Run: `bazel test //tests:test_checksum_results --test_output=errors`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add modules/checksum/checksum_ecu_subaru_denso_sh7xxx.cpp
git commit -m "refactor: checksum_ecu_subaru_denso_sh7xxx.cpp uses bytes:: helpers"
```

---

## Final verification (after all 21 tasks)

- [ ] Run the full test suite: `bazel test //tests/...`
- [ ] Expected: all suites PASS, none newly failing or newly skipped.
- [ ] Grep for stragglers: `grep -rn "<< *24\|<< *16\|<< *8" --include=*.cpp ecu_operations.cpp dtc_operations.cpp get_key_operations_subaru.cpp serial_port/serial_port_actions_direct.cpp protocol/fastecu_can_transport.cpp protocol/mitsu_colt_can_cdbg_protocol.cpp modules/ssm_protocol.cpp modules/flash_utils.cpp modules/biu/biu_operations_subaru.cpp modules/checksum/` and confirm every remaining hit is one of the documented out-of-scope cases (algorithm-internal bit manipulation, or the `word0`/`word1` formula in `seedToKey`).

---

## Execution outcome (2026-07-10)

All 21 tasks executed via subagent-driven development, each with a task-scoped
review; final whole-branch review completed. `bazel test //tests/...` (all 30
targets) passes clean. The stragglers grep above returns only documented
out-of-scope hits (commented-out dead code, algorithm-internal crypto/scramble
logic, and the explicitly-excluded `seedToKey` word0/word1 formula, checksum
6's single-byte accumulation, and `decodeFrame`'s variable-width branch).

Two corrections made during execution:
- **Task 4** (`ecu_operations.cpp`): the plan's own prescribed code for
  `read_mem_32bit_can`/`reflash_block_32bit_can` was wrong — `writeU24Be`
  needed a `>> 8` pre-shift since the original truncated the top 24 of a
  32-bit value, not the bottom 24. Caught by task review, fixed in both the
  code and this plan document.
- **Final review**: `checksum_ecu_subaru_hitachi_m32r_can.cpp`'s checksum-5
  and `checksum_ecu_subaru_hitachi_m32r_kline.cpp`'s checksum-3 balance-value
  reads changed observable output for balance bytes with the low byte >=
  0x80 — the original used an unmasked signed-char cast that disagreed with
  the sibling `*_stored` read of the same bytes (already unsigned).
  `bytes::readU16Be` makes both reads consistent. Confirmed with the user
  and accepted as an intentional fix (documented inline at both sites,
  commit `3ddc392`), since the original's inconsistency was itself the
  latent bug.

Wave 2 (the ~19 untested Subaru per-variant ECU/TCU/EEPROM/JTAG files, plus
the ~80 protocol-constant-splitting comparisons) remains a separate,
not-yet-written follow-up plan.
