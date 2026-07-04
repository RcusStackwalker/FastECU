# Mitsu Colt CAN Vendor-Extension Challenge Algorithm Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the seed/key challenge found on a commercially-tuned `47110032` (Colt CZT, Z37A) ROM into a pure, testable protocol module in FastECU — both the forward transform (as it exists in ROM) and its inverse (what a real client actually needs to answer the challenge).

**Architecture:** A new `protocol/mitsu_colt_can_vendor_ext_protocol.{h,cpp}` pair, following the existing `MitsuColtCan`/`MitsuColtCanCdbg` convention exactly: a pure-logic namespace with no I/O, no Qt widgets, no session state. Three pieces: the forward bit-mixing transform (ported verbatim from ROM offset `0x510b8`), its analytically-derived inverse (the function a client calls), and big-endian byte-array convenience wrappers matching the 4-byte wire layout.

**Tech Stack:** C++17, Qt (`QByteArray`, `qint`/`quint` types), `qmake`, QtTest.

## Global Constraints

- Pure logic only — no CAN/K-line framing, no session-state tracking, no I/O. (Spec "Scope", out-of-scope list.)
- Namespace `MitsuColtCanVendorExt`; files `protocol/mitsu_colt_can_vendor_ext_protocol.h` / `.cpp` — vendor-neutral naming, no third-party product name in code/file identifiers (comments may reference it factually). (Spec "File layout & API".)
- `challengeInverseTransform` is the function a real client calls — NOT `challengeTransform` re-applied. A simpler direct-memory-read shortcut was investigated and confirmed not to work (generic ECU reads re-advance the secret immediately after replying); it is documented in the spec as a rejected approach and is out of scope here. (Spec "Rejected approach".)
- Tests go in `tests/test_mitsu_colt_can_vendor_ext_protocol.{h,cpp}`, QtTest, matching the shape of the existing `tests/test_mitsu_colt_can_protocol.cpp`. (Spec "Testing".)
- Full spec: `docs/superpowers/specs/2026-07-04-mitsu-colt-can-vendor-ext-challenge-design.md`.

---

## Pre-verified facts (do not re-derive)

The exhaustive verification the spec calls for has already been run (a throwaway C program, not part of this plan's deliverables) — `challengeInverseTransform(challengeTransform(x)) == x` holds for **all 4,294,967,296 possible 32-bit values, 0 mismatches**. The five vectors below come directly from that run and are safe to hardcode into tests:

| secret (input to `challengeTransform`) | seed = `challengeTransform(secret)` |
|---|---|
| `0x00000000` | `0xF2E207C5` |
| `0xFFFFFFFF` | `0xE4C2C64D` |
| `0x12345678` | `0x669E0CB4` |
| `0x00000001` | `0xE0E207D2` |
| `0xDEADBEEF` | `0xA5654127` |

And `challengeInverseTransform(seed) == secret` for each row above (that's what "0 mismatches" means).

---

### Task 1: Forward transform (`challengeTransform`) + build scaffolding

**Files:**
- Create: `protocol/mitsu_colt_can_vendor_ext_protocol.h`
- Create: `protocol/mitsu_colt_can_vendor_ext_protocol.cpp`
- Create: `tests/test_mitsu_colt_can_vendor_ext_protocol.h`
- Create: `tests/test_mitsu_colt_can_vendor_ext_protocol.cpp`
- Modify: `FastECU.pro:162` (SOURCES), `FastECU.pro:258` (HEADERS)
- Modify: `tests/tests.pro:37` (SOURCES, test file), `tests/tests.pro:62` (SOURCES, protocol file), `tests/tests.pro:85` (HEADERS, test file)
- Modify: `tests/main.cpp:7` (include), `tests/main.cpp:25` (registration)

**Interfaces:**
- Produces: `quint32 MitsuColtCanVendorExt::challengeTransform(quint32 secret)` — used by Task 2's tests (round-trip check) and Task 2 does not modify this function.

- [ ] **Step 1: Create the protocol header**

Write `protocol/mitsu_colt_can_vendor_ext_protocol.h`:

```cpp
#pragma once
#include <QByteArray>

// Mitsubishi Colt CZT (Z37A, ROM 47110032) vendor diagnostic-extension
// challenge algorithm. Reverse-engineered by static disassembly of a
// commercially-tuned ROM's diagnostic-dispatcher hijack (not from any
// vendor source or documentation) — see
// docs/superpowers/specs/2026-07-04-mitsu-colt-can-vendor-ext-challenge-design.md
// for the full derivation.
//
// This challenge gates entry into the 0x85 ("bootload") diagnostic session
// on ROMs carrying this specific third-party extension. It is unrelated to
// MitsuColtCan::seedKey (the ECU's own factory SecurityAccess) and to
// MitsuColtCanCdbg (livemonitor's separate live-logging protocol, CAN
// 0x630/0x631) — only ROMs with this vendor extension present use it; stock
// ROMs and this project's own patches never touch it.
//
// Pure, hardware-independent functions only — no I/O here.
namespace MitsuColtCanVendorExt {

// Forward transform, ported verbatim from ROM 47110032 offset 0x510b8. This
// is what the ECU applies to its internal secret to produce the seed value
// it sends the client. Provided for documentation/completeness and as the
// basis for the round-trip check in tests — not what a client calls.
quint32 challengeTransform(quint32 secret);

} // namespace MitsuColtCanVendorExt
```

- [ ] **Step 2: Create the protocol source file**

Write `protocol/mitsu_colt_can_vendor_ext_protocol.cpp`:

```cpp
#include "protocol/mitsu_colt_can_vendor_ext_protocol.h"

namespace MitsuColtCanVendorExt {

namespace {

// Bit permutation used at the start of every round. Confirmed to partition
// all 32 bits with zero overlap — see the design doc for the derivation.
// It is its own inverse (a permutation built purely of 2-cycles).
quint32 permuteBits(quint32 x) {
    return ((x & 0x15555555u) << 3)
         | ((x & 0xaaaaaaa8u) >> 3)
         | ((x & 0x40000000u) >> 29)
         | ((x & 0x00000002u) << 29);
}

// Nibble swap within each byte. Also its own inverse: applying it twice is
// a no-op.
quint32 swapNibbles(quint32 x) {
    return ((x & 0x0f0f0f0fu) << 4) | ((x & 0xf0f0f0f0u) >> 4);
}

// Round constants read from ROM flash at 0x5fddc..0x5fddc+15 (big-endian
// u32 each) — literally the first 16 bytes of the "EcuTek ROM Patch" ASCII
// string embedded elsewhere in the same ROM, reinterpreted as words.
constexpr quint32 kRoundConstants[4] = {
    0x45637554u, // "EcuT"
    0x656b2052u, // "ek R"
    0x4f4d2050u, // "OM P"
    0x61746368u, // "atch"
};

} // namespace

quint32 challengeTransform(quint32 secret) {
    quint32 x = secret;
    for (int i = 0; i < 4; ++i) {
        x = swapNibbles(permuteBits(x) + kRoundConstants[i]);
    }
    return x;
}

} // namespace MitsuColtCanVendorExt
```

- [ ] **Step 3: Create the test header**

Write `tests/test_mitsu_colt_can_vendor_ext_protocol.h`:

```cpp
#pragma once
int run_test_mitsu_colt_can_vendor_ext_protocol(int argc, char** argv);
```

- [ ] **Step 4: Write the failing test**

Write `tests/test_mitsu_colt_can_vendor_ext_protocol.cpp`:

```cpp
#include <QtTest>
#include "protocol/mitsu_colt_can_vendor_ext_protocol.h"
#include "test_mitsu_colt_can_vendor_ext_protocol.h"
using namespace MitsuColtCanVendorExt;

class TestMitsuColtCanVendorExtProtocol : public QObject { Q_OBJECT
private slots:
    void challenge_transform_matches_known_vectors() {
        // Vectors confirmed by exhaustive brute-force cross-check against
        // challengeInverseTransform() across the full 32-bit domain — 0
        // mismatches out of 4294967296. See design doc "Pre-verified facts".
        QCOMPARE(challengeTransform(0x00000000u), quint32(0xF2E207C5u));
        QCOMPARE(challengeTransform(0xFFFFFFFFu), quint32(0xE4C2C64Du));
        QCOMPARE(challengeTransform(0x12345678u), quint32(0x669E0CB4u));
        QCOMPARE(challengeTransform(0x00000001u), quint32(0xE0E207D2u));
        QCOMPARE(challengeTransform(0xDEADBEEFu), quint32(0xA5654127u));
    }
};
int run_test_mitsu_colt_can_vendor_ext_protocol(int argc, char** argv) {
    TestMitsuColtCanVendorExtProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_mitsu_colt_can_vendor_ext_protocol.moc"
```

(This test can't run yet — the build files below haven't wired it in. That's the next steps.)

- [ ] **Step 5: Wire the protocol file into the main app build**

In `FastECU.pro`, find this line (around line 162):

```
    protocol/mitsu_colt_can_protocol.cpp \
```

Add immediately after it:

```
    protocol/mitsu_colt_can_vendor_ext_protocol.cpp \
```

Find this line (around line 258):

```
    protocol/mitsu_colt_can_protocol.h \
```

Add immediately after it:

```
    protocol/mitsu_colt_can_vendor_ext_protocol.h \
```

- [ ] **Step 6: Wire the protocol and test files into the test build**

In `tests/tests.pro`, find this line (around line 37):

```
    test_mitsu_colt_can_protocol.cpp \
```

Add immediately after it:

```
    test_mitsu_colt_can_vendor_ext_protocol.cpp \
```

Find this line (around line 62):

```
    ../protocol/mitsu_colt_can_protocol.cpp \
```

Add immediately after it:

```
    ../protocol/mitsu_colt_can_vendor_ext_protocol.cpp \
```

Find this line (around line 85):

```
    test_mitsu_colt_can_protocol.h \
```

Add immediately after it:

```
    test_mitsu_colt_can_vendor_ext_protocol.h \
```

- [ ] **Step 7: Register the test in `main.cpp`**

In `tests/main.cpp`, find this line (around line 7):

```cpp
#include "test_mitsu_colt_can_protocol.h"
```

Add immediately after it:

```cpp
#include "test_mitsu_colt_can_vendor_ext_protocol.h"
```

Find this line (around line 25):

```cpp
    status |= run_test_mitsu_colt_can_protocol(argc, argv);
```

Add immediately after it:

```cpp
    status |= run_test_mitsu_colt_can_vendor_ext_protocol(argc, argv);
```

- [ ] **Step 8: Build and run — verify the new test passes**

```bash
cd tests && qmake && make && ./mut_dma_tests
```

Expected: build succeeds, and in the output a line like:
```
PASS   : TestMitsuColtCanVendorExtProtocol::challenge_transform_matches_known_vectors()
```
with no `FAIL` for `TestMitsuColtCanVendorExtProtocol`. (Other pre-existing test suites in the same binary run too — ignore those, just confirm this one passes.)

- [ ] **Step 9: Commit**

```bash
git add protocol/mitsu_colt_can_vendor_ext_protocol.h protocol/mitsu_colt_can_vendor_ext_protocol.cpp \
        tests/test_mitsu_colt_can_vendor_ext_protocol.h tests/test_mitsu_colt_can_vendor_ext_protocol.cpp \
        FastECU.pro tests/tests.pro tests/main.cpp
git commit -m "feat: port vendor diagnostic-extension challenge forward transform

Ported from ROM 47110032 offset 0x510b8 (static disassembly of a
commercially-tuned ROM's diagnostic-dispatcher hijack). Forward direction
only in this commit; the inverse a real client needs follows next."
```

---

### Task 2: Inverse transform (`challengeInverseTransform`)

**Files:**
- Modify: `protocol/mitsu_colt_can_vendor_ext_protocol.h`
- Modify: `protocol/mitsu_colt_can_vendor_ext_protocol.cpp`
- Modify: `tests/test_mitsu_colt_can_vendor_ext_protocol.cpp`

**Interfaces:**
- Consumes: `permuteBits(quint32)`, `swapNibbles(quint32)`, `kRoundConstants[4]` (all defined in the anonymous namespace of `protocol/mitsu_colt_can_vendor_ext_protocol.cpp` by Task 1) — both are involutions (self-inverse), which is why the inverse reuses them unchanged rather than needing separate inverse permutation functions.
- Produces: `quint32 MitsuColtCanVendorExt::challengeInverseTransform(quint32 seed)` — this is the function later integration work (out of scope here) will actually call.

- [ ] **Step 1: Write the failing tests**

In `tests/test_mitsu_colt_can_vendor_ext_protocol.cpp`, add two new test methods inside `TestMitsuColtCanVendorExtProtocol`, right after `challenge_transform_matches_known_vectors()`:

```cpp
    void challenge_inverse_transform_matches_known_vectors() {
        // Same five vectors as the forward test, inverted.
        QCOMPARE(challengeInverseTransform(0xF2E207C5u), quint32(0x00000000u));
        QCOMPARE(challengeInverseTransform(0xE4C2C64Du), quint32(0xFFFFFFFFu));
        QCOMPARE(challengeInverseTransform(0x669E0CB4u), quint32(0x12345678u));
        QCOMPARE(challengeInverseTransform(0xE0E207D2u), quint32(0x00000001u));
        QCOMPARE(challengeInverseTransform(0xA5654127u), quint32(0xDEADBEEFu));
    }
    void challenge_inverse_round_trips_with_forward() {
        // Lightweight regression check standing in for the one-time
        // exhaustive 2^32 proof recorded in the design doc.
        const quint32 values[] = {0x00000000u, 0xFFFFFFFFu, 0x12345678u,
                                   0x00000001u, 0xDEADBEEFu, 0x80000000u, 0x7FFFFFFFu};
        for (quint32 x : values) {
            QCOMPARE(challengeInverseTransform(challengeTransform(x)), x);
        }
    }
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cd tests && qmake && make && ./mut_dma_tests
```

Expected: compile error — `challengeInverseTransform` is not declared (it doesn't exist yet). This is the "fails" state; it's a compile failure rather than a runtime assertion failure, which is normal for C++ when the symbol itself doesn't exist yet.

- [ ] **Step 3: Declare the inverse in the header**

In `protocol/mitsu_colt_can_vendor_ext_protocol.h`, add this declaration right after `challengeTransform`'s declaration (before the closing `} // namespace MitsuColtCanVendorExt`):

```cpp

// Inverse of challengeTransform(), analytically derived (see design doc
// for the derivation) and verified against the forward function across the
// full 32-bit domain. THIS is what a real client calls: given the 4-byte
// seed the ECU sends, computes the key value the ECU will accept.
quint32 challengeInverseTransform(quint32 seed);
```

- [ ] **Step 4: Implement the inverse**

In `protocol/mitsu_colt_can_vendor_ext_protocol.cpp`, add this function right after `challengeTransform`'s closing brace (still inside `namespace MitsuColtCanVendorExt { ... }`, after the anonymous namespace helpers are already in scope):

```cpp

quint32 challengeInverseTransform(quint32 seed) {
    quint32 x = seed;
    for (int i = 3; i >= 0; --i) {
        x = permuteBits(swapNibbles(x) - kRoundConstants[i]);
    }
    return x;
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd tests && qmake && make && ./mut_dma_tests
```

Expected: both new test methods `PASS`:
```
PASS   : TestMitsuColtCanVendorExtProtocol::challenge_inverse_transform_matches_known_vectors()
PASS   : TestMitsuColtCanVendorExtProtocol::challenge_inverse_round_trips_with_forward()
```

- [ ] **Step 6: Commit**

```bash
git add protocol/mitsu_colt_can_vendor_ext_protocol.h protocol/mitsu_colt_can_vendor_ext_protocol.cpp \
        tests/test_mitsu_colt_can_vendor_ext_protocol.cpp
git commit -m "feat: add vendor diagnostic-extension challenge inverse transform

The ECU compares the client's key against the raw pre-transform secret, not
the transformed seed it sends — a client must invert the transform, not
reapply it. Both P1 and P2 (the permutation and nibble-swap steps) are
involutions, so the inverse reuses them unchanged, undoing round constants
in reverse order. Verified against the forward function across the full
32-bit domain (0 mismatches / 4294967296) prior to this commit."
```

---

### Task 3: Byte-array wrappers (`bytesToSeed` / `keyToBytes`)

**Files:**
- Modify: `protocol/mitsu_colt_can_vendor_ext_protocol.h`
- Modify: `protocol/mitsu_colt_can_vendor_ext_protocol.cpp`
- Modify: `tests/test_mitsu_colt_can_vendor_ext_protocol.cpp`

**Interfaces:**
- Consumes: nothing from Tasks 1–2 beyond the `quint32`-based functions already in place; this task is purely packing/unpacking bytes around them.
- Produces: `quint32 MitsuColtCanVendorExt::bytesToSeed(const QByteArray &seedBytes)`, `QByteArray MitsuColtCanVendorExt::keyToBytes(quint32 key)` — these are the last pieces of this module's public API; nothing later in this plan depends on them (deferred integration work, out of scope, will).

- [ ] **Step 1: Write the failing tests**

In `tests/test_mitsu_colt_can_vendor_ext_protocol.cpp`, add one more test method after `challenge_inverse_round_trips_with_forward()`:

```cpp
    void byte_wrappers_round_trip() {
        QByteArray seedBytes = QByteArray::fromHex("F2E207C5");
        QCOMPARE(bytesToSeed(seedBytes), quint32(0xF2E207C5u));
        QCOMPARE(keyToBytes(0xF2E207C5u), seedBytes);
        // Round trip through the real challenge functions too.
        quint32 secret = 0x12345678u;
        QByteArray onWire = keyToBytes(challengeTransform(secret));
        QCOMPARE(challengeInverseTransform(bytesToSeed(onWire)), secret);
    }
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cd tests && qmake && make && ./mut_dma_tests
```

Expected: compile error — `bytesToSeed`/`keyToBytes` not declared.

- [ ] **Step 3: Declare the wrappers in the header**

In `protocol/mitsu_colt_can_vendor_ext_protocol.h`, add after `challengeInverseTransform`'s declaration (still before the closing `} // namespace MitsuColtCanVendorExt`):

```cpp

// Big-endian byte-array convenience wrappers matching the 4-byte wire
// layout, mirroring MitsuColtCan::seedKey's shape.
quint32 bytesToSeed(const QByteArray &seedBytes);  // expects exactly 4 bytes
QByteArray keyToBytes(quint32 key);                 // produces exactly 4 bytes
```

- [ ] **Step 4: Implement the wrappers**

In `protocol/mitsu_colt_can_vendor_ext_protocol.cpp`, add after `challengeInverseTransform`'s closing brace:

```cpp

quint32 bytesToSeed(const QByteArray &seedBytes) {
    Q_ASSERT(seedBytes.size() == 4);
    return (quint32(quint8(seedBytes.at(0))) << 24)
         | (quint32(quint8(seedBytes.at(1))) << 16)
         | (quint32(quint8(seedBytes.at(2))) << 8)
         |  quint32(quint8(seedBytes.at(3)));
}

QByteArray keyToBytes(quint32 key) {
    QByteArray bytes;
    bytes.append(char((key >> 24) & 0xFF));
    bytes.append(char((key >> 16) & 0xFF));
    bytes.append(char((key >> 8) & 0xFF));
    bytes.append(char(key & 0xFF));
    return bytes;
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd tests && qmake && make && ./mut_dma_tests
```

Expected: `PASS   : TestMitsuColtCanVendorExtProtocol::byte_wrappers_round_trip()`.

- [ ] **Step 6: Run the full test suite once more, standalone, to confirm nothing else broke**

```bash
cd tests && ./mut_dma_tests 2>&1 | grep -E "FAIL|Totals"
```

Expected: no `FAIL` lines; the totals line reports 0 failures across the whole binary (this new suite plus every pre-existing one).

- [ ] **Step 7: Commit**

```bash
git add protocol/mitsu_colt_can_vendor_ext_protocol.h protocol/mitsu_colt_can_vendor_ext_protocol.cpp \
        tests/test_mitsu_colt_can_vendor_ext_protocol.cpp
git commit -m "feat: add byte-array wrappers for vendor challenge algorithm

Completes the pure-logic extraction: challengeTransform (forward, verbatim
from ROM), challengeInverseTransform (what a real client calls), and
big-endian 4-byte wire packing/unpacking. Wiring this into an actual
session handshake is deferred to a later integration session (see design
doc's out-of-scope list)."
```
