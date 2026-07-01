# Colt CZT (Z37A, 47110032) CAN Reflash Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the working CAN-based reflash protocol for the Mitsubishi Colt CZT (Z37A, ROM `47110032`) from `externals/livemonitor` into `externals/FastECU`, so FastECU becomes the actively maintained, cross-platform tool for reading and writing this ECU over CAN.

**Architecture:** A new pure-logic protocol library (`protocol/mitsu_colt_can_protocol.{h,cpp}`) implements the Mitsubishi-specific seed-key algorithm, checksum, and UDS-style frame builders as free functions, fully unit-tested without hardware. A new QDialog module (`modules/ecu/flash_ecu_mitsu_m32r_can.{h,cpp}`), modeled on `modules/ecu/flash_ecu_subaru_hitachi_m32r_can.cpp`'s structure, calls those frame builders and drives them over FastECU's existing blocking `SerialPortActions` API with `flash_transport=iso15765` — no new CAN/ISO-TP transport code is needed, since the J2534 driver already handles ISO-TP framing in that mode. The module is wired into the existing `config/protocols.cfg` → `mainwindow.cpp` dispatch chain like every other ECU module.

**Tech Stack:** Qt 6 (C++), qmake, QTest (existing `tests/` harness), J2534 PassThru via FastECU's `SerialPortActions`.

## Global Constraints

- Target ROM is `47110032` (Mitsubishi Colt CZT, Z37A, MCU `M32176F3`, 384KB flash) only. Do not generalize to other Colt ROMs.
- ISO15765 request CAN ID `0x7E0`, reply CAN ID `0x7E8` (confirmed from `livemonitor/mainwindow.cpp:43,50`).
- Writable "userspace" region is `0x008000`–`0x060000` (352KB); `0x000000`–`0x008000` is the protected boot region and must never be written.
- `test_write` is not implemented for this protocol (`<test_write>no</test_write>`), matching every other M32R protocol in FastECU. Do not build a dry-run.
- The erase-trigger step (Task 6) is a known landmine: the original livemonitor author's own comment says it caused a bootloader lockup during their testing. It must ship behind an explicit in-UI confirmation dialog, never sent silently.
- Every outgoing ISO15765 payload in this codebase's CAN modules is prefixed with 4 bytes encoding the destination CAN ID big-endian (`0x00 0x00 0x07 0xE0` for `0x7E0`) before the actual UDS/SID bytes — confirmed by inspecting `modules/ecu/flash_ecu_subaru_hitachi_m32r_can.cpp`. Replies are similarly prefixed, so the first SID byte of a reply is at `received.at(4)`.
- New source files must be added to `FastECU.pro`'s `SOURCES`/`HEADERS` lists (qmake, no auto-glob) and `tests/tests.pro` for the protocol library's tests.

---

### Task 1: Protocol library — seed-key, checksum, and UDS frame builders

**Files:**
- Create: `externals/FastECU/protocol/mitsu_colt_can_protocol.h`
- Create: `externals/FastECU/protocol/mitsu_colt_can_protocol.cpp`
- Create: `externals/FastECU/tests/test_mitsu_colt_can_protocol.h`
- Create: `externals/FastECU/tests/test_mitsu_colt_can_protocol.cpp`
- Modify: `externals/FastECU/tests/tests.pro`
- Modify: `externals/FastECU/tests/main.cpp`

**Interfaces:**
- Produces (used by Task 3/4/5/6's `modules/ecu/flash_ecu_mitsu_m32r_can.cpp`): everything declared in `mitsu_colt_can_protocol.h`, namespace `MitsuColtCan` — listed in full in Step 3 below.

- [ ] **Step 1: Write the failing test file**

Create `externals/FastECU/tests/test_mitsu_colt_can_protocol.h`:

```cpp
#pragma once
int run_test_mitsu_colt_can_protocol(int argc, char** argv);
```

Create `externals/FastECU/tests/test_mitsu_colt_can_protocol.cpp`:

```cpp
#include <QtTest>
#include "protocol/mitsu_colt_can_protocol.h"
#include "test_mitsu_colt_can_protocol.h"
using namespace MitsuColtCan;

class TestMitsuColtCanProtocol : public QObject { Q_OBJECT
private slots:
    void seed_key_word_matches_known_vectors() {
        // sk = (pk * 135 + 1542) mod 65536, hand-computed from
        // externals/livemonitor/obdengine.cpp's ObdSessionInitCommandSequence.
        QCOMPARE(seedKeyWord(0x0000), quint16(0x0606));
        QCOMPARE(seedKeyWord(0x1234), quint16(0x9F72));
        QCOMPARE(seedKeyWord(0xFFFF), quint16(0x057F));
    }
    void seed_key_combines_both_halves() {
        QByteArray seed = QByteArray::fromHex("12340000");
        QByteArray key = seedKey(seed);
        QCOMPARE(key.size(), 4);
        QCOMPARE(quint8(key.at(0)), quint8(0x9F));
        QCOMPARE(quint8(key.at(1)), quint8(0x72));
        QCOMPARE(quint8(key.at(2)), quint8(0x06));
        QCOMPARE(quint8(key.at(3)), quint8(0x06));
    }
    void checksum_sums_every_byte_with_wraparound() {
        QCOMPARE(checksum(QByteArray::fromHex("01020304")), quint16(0x000A));
        QCOMPARE(checksum(QByteArray(200, char(0xFF))), quint16(0xC738));
    }
    void request_download_frame_layout() {
        QByteArray f = buildRequestDownloadFrame(0x008000, 0x000100);
        QCOMPARE(f, QByteArray::fromHex("3400800000000100"));
    }
    void transfer_data_frames_chunk_at_256_bytes() {
        QByteArray payload(300, char(0x5A));
        QVector<QByteArray> frames = buildTransferDataFrames(payload);
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames.at(0).size(), 257); // SID + 256 bytes
        QCOMPARE(quint8(frames.at(0).at(0)), quint8(kServiceTransferData));
        QCOMPARE(frames.at(1).size(), 45);  // SID + 44 remaining bytes
    }
    void routine_check_crc_selects_flash_vs_memory() {
        QCOMPARE(buildRoutineCheckCrcFrame(0x008000), QByteArray::fromHex("31E102")); // < 0x800000 -> flash (2)
        QCOMPARE(buildRoutineCheckCrcFrame(0x805568), QByteArray::fromHex("31E101")); // >= 0x800000 -> memory (1)
    }
    void routine_erase_frame_is_two_bytes_no_selector() {
        QCOMPARE(buildRoutineEraseFrame(), QByteArray::fromHex("31E0"));
    }
    void request_reflash_unlock_frame_matches_source_bytes() {
        // Ported verbatim from externals/livemonitor/obdsessionwidget.cpp:180-181.
        // The original author's comment on this exact payload: "caused bootloader lockup".
        QByteArray expected = QByteArray::fromHex("3B9A01015263757330300001");
        QCOMPARE(buildRequestReflashUnlockFrame(), expected);
    }
    void read_memory_by_address_frame_layout() {
        QCOMPARE(buildReadMemoryByAddressFrame(0x008000, 192), QByteArray::fromHex("23008000C0"));
    }
    void diagnostic_session_frame_layout() {
        QCOMPARE(buildDiagnosticSessionFrame(kSessionBootload), QByteArray::fromHex("1085"));
        QCOMPARE(buildDiagnosticSessionFrame(kSessionBasic), QByteArray::fromHex("1081"));
    }
    void security_access_frame_layout() {
        QCOMPARE(buildSecurityAccessSeedRequestFrame(), QByteArray::fromHex("2705"));
        QByteArray key = QByteArray::fromHex("9F720606");
        QCOMPARE(buildSecurityAccessKeyFrame(key), QByteArray::fromHex("2706") + key);
    }
    void erase_and_write_page_routines_match_colt_flasher_xml_sizes() {
        QCOMPARE(sizeof(kErasePageRoutine), size_t(160));
        QCOMPARE(sizeof(kWritePageRoutine), size_t(176));
        QCOMPARE(kErasePageRoutine[0], quint8(0x94));
        QCOMPARE(kWritePageRoutine[0], quint8(0x94));
    }
};
int run_test_mitsu_colt_can_protocol(int argc, char** argv) {
    TestMitsuColtCanProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_mitsu_colt_can_protocol.moc"
```

- [ ] **Step 2: Wire the new test into the test harness build**

In `externals/FastECU/tests/tests.pro`, add the new files to `SOURCES` and `HEADERS`:

```diff
 SOURCES += \
     main.cpp \
     test_codec.cpp \
     test_freeform.cpp \
     test_memory.cpp \
     test_transport.cpp \
     test_init.cpp \
     test_driver.cpp \
+    test_mitsu_colt_can_protocol.cpp \
+    ../protocol/mitsu_colt_can_protocol.cpp \
     ../protocol/mut_dma_codec.cpp \
     ../protocol/mut_dma_freeform.cpp \
     ../protocol/mut_dma_memory.cpp \
     ../protocol/imut_dma_init.cpp \
     ../protocol/mut_dma_driver.cpp
 HEADERS += \
     test_codec.h \
     test_freeform.h \
     test_memory.h \
     test_transport.h \
     test_init.h \
     test_driver.h \
+    test_mitsu_colt_can_protocol.h \
     scripted_kline_transport.h \
     ../protocol/ikline_transport.h
```

In `externals/FastECU/tests/main.cpp`:

```diff
 #include "test_codec.h"
 #include "test_freeform.h"
 #include "test_memory.h"
 #include "test_transport.h"
 #include "test_init.h"
 #include "test_driver.h"
+#include "test_mitsu_colt_can_protocol.h"

 int main(int argc, char** argv) {
     int status = 0;
     status |= run_test_codec(argc, argv);
     status |= run_test_freeform(argc, argv);
     status |= run_test_memory(argc, argv);
     status |= run_test_transport(argc, argv);
     status |= run_test_init(argc, argv);
     status |= run_test_driver(argc, argv);
+    status |= run_test_mitsu_colt_can_protocol(argc, argv);
     return status;
 }
```

- [ ] **Step 3: Run the test build to verify it fails**

Run:
```bash
cd externals/FastECU/tests && qmake tests.pro && make 2>&1 | tail -30
```
Expected: FAIL — compile error, `protocol/mitsu_colt_can_protocol.h: No such file or directory` (it doesn't exist yet).

- [ ] **Step 4: Write the protocol library header**

Create `externals/FastECU/protocol/mitsu_colt_can_protocol.h`:

```cpp
#pragma once
#include <QByteArray>
#include <QVector>

// Mitsubishi Colt CZT (Z37A, ROM 47110032) CAN reflash protocol.
// Ported from externals/livemonitor/obdengine.cpp + colt_flasher.xml.
// Request CAN ID 0x7E0, reply CAN ID 0x7E8 (ISO 15765 / UDS-style OBD).
// Pure, hardware-independent frame builders only — no I/O here.
namespace MitsuColtCan {

constexpr quint8 kServiceDiagnosticSession    = 0x10;
constexpr quint8 kServiceSecurityAccess       = 0x27;
constexpr quint8 kServiceReadMemoryByAddress  = 0x23;
constexpr quint8 kServiceRequestDownload      = 0x34;
constexpr quint8 kServiceTransferData         = 0x36;
constexpr quint8 kServiceRoutineControl       = 0x31;
constexpr quint8 kServiceRequestReflash       = 0x3B;

constexpr quint8 kSessionBasic    = 0x81;
constexpr quint8 kSessionBootload = 0x85;

constexpr quint32 kCrcTransferAddress = 0x200000;
constexpr quint32 kCrcTransferSize    = 2;
constexpr quint8  kRoutineCheckCrc    = 225;
constexpr quint8  kRoutineErase       = 224;

constexpr quint32 kUserspaceStart      = 0x008000;
constexpr quint32 kUserspaceEnd        = 0x060000;
constexpr quint32 kFlashReadBlockSize  = 192;
constexpr quint32 kTransferChunkSize   = 256;

constexpr quint32 kEraseRoutineRamAddr = 0x805568;
constexpr quint32 kWriteRoutineRamAddr = 0x8054AC;

// RAM-resident erase/write helper routines, carried over verbatim (not
// recompiled) from externals/livemonitor/colt_flasher.xml. Valid only for
// ROM 47110032 (Colt CZT, Z37A).
extern const quint8 kErasePageRoutine[160];
extern const quint8 kWritePageRoutine[176];

// sk = (pk * 135 + 1542) mod 65536. Mitsubishi-specific; ported from
// ObdSessionInitCommandSequence::messageCallbackCustomAction in
// externals/livemonitor/obdengine.cpp. Different from (and not compatible
// with) the table-based algorithms used by the Subaru-targeted modules in
// this codebase.
quint16 seedKeyWord(quint16 seedWord);

// Applies seedKeyWord() to both 16-bit halves of a 4-byte seed (big-endian
// in, big-endian out): seed = [pk1_hi, pk1_lo, pk2_hi, pk2_lo].
QByteArray seedKey(const QByteArray &seed);

// 16-bit running sum of every raw byte, with natural wraparound. Matches
// get_crc() in externals/livemonitor/obdengine.cpp — not a real CRC.
quint16 checksum(const QByteArray &data);

// SID 0x34: [SID][start>>16][start>>8][start][0x00][size>>16][size>>8][size].
QByteArray buildRequestDownloadFrame(quint32 start, quint32 size);

// SID 0x36, chunked at kTransferChunkSize bytes per frame: [SID][up to 256 payload bytes].
QVector<QByteArray> buildTransferDataFrames(const QByteArray &payload);

// SID 0x31/225: [SID][225][selector]. selector = 2 if targetStart < 0x800000
// ("flash"), else 1 ("memory") — matches obdengine.cpp's "flash/memory fun".
QByteArray buildRoutineCheckCrcFrame(quint32 targetStart);

// SID 0x31/224, bare 2 bytes, no selector byte. Source comment: "causes reflash".
QByteArray buildRoutineEraseFrame();

// SID 0x3B, 12-byte payload ported verbatim from
// externals/livemonitor/obdsessionwidget.cpp:180-181. KNOWN RISK: the
// original author's comment on this exact payload reads "caused bootloader
// lockup" during their own testing. Treat any call site as the highest-risk
// step in this protocol; never send without an explicit confirmation gate
// on a bench/spare ECU. See docs/superpowers/specs/2026-06-30-fastecu-colt-can-reflash-design.md.
QByteArray buildRequestReflashUnlockFrame();

// SID 0x23: [SID][addr>>16][addr>>8][addr][len].
QByteArray buildReadMemoryByAddressFrame(quint32 addr, quint8 len);

// SID 0x10: [SID][sessionId].
QByteArray buildDiagnosticSessionFrame(quint8 sessionId);

// SID 0x27/5 (seed request): [SID][0x05].
QByteArray buildSecurityAccessSeedRequestFrame();

// SID 0x27/6 (key answer): [SID][0x06][4-byte key].
QByteArray buildSecurityAccessKeyFrame(const QByteArray &key);

} // namespace MitsuColtCan
```

- [ ] **Step 5: Write the protocol library implementation**

Create `externals/FastECU/protocol/mitsu_colt_can_protocol.cpp`. Transcribe the `erasepage`/`writepage` blobs byte-for-byte from `externals/livemonitor/colt_flasher.xml` (16 bytes per source line, preserved below for easy visual diff against the XML):

```cpp
#include "protocol/mitsu_colt_can_protocol.h"

namespace MitsuColtCan {

const quint8 kErasePageRoutine[160] = {
    0x94, 0xf0, 0x20, 0x20, 0x24, 0x20, 0xf0, 0x00, 0xe4, 0x00, 0xd0, 0xd0, 0x24, 0x20, 0x64, 0x32,
    0x44, 0xff, 0xf0, 0x00, 0xb0, 0x94, 0xff, 0xff, 0xd8, 0xc0, 0x01, 0x6e, 0x88, 0xe8, 0x36, 0x00,
    0xe1, 0x80, 0x56, 0x54, 0x1e, 0xc1, 0xf0, 0x00, 0xe1, 0x80, 0x07, 0xe1, 0x21, 0x91, 0xf0, 0x00,
    0x81, 0xc1, 0x00, 0x01, 0xb0, 0x91, 0x00, 0x18, 0x48, 0xff, 0xf0, 0x00, 0xb0, 0x98, 0xff, 0xf9,
    0xe2, 0xff, 0xff, 0xff, 0xe2, 0xff, 0xff, 0xff, 0xe2, 0xff, 0xff, 0xff, 0xe2, 0xff, 0xff, 0xff,
    0xe2, 0xff, 0xff, 0xef, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00,
    0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00,
    0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00,
    0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00,
    0x70, 0x00, 0x70, 0x00, 0x60, 0x01, 0xf0, 0x00, 0xb0, 0x98, 0x00, 0x02, 0x60, 0x00, 0xf0, 0x00,
};

const quint8 kWritePageRoutine[176] = {
    0x94, 0xf0, 0x41, 0x41, 0x24, 0x20, 0xf0, 0x00, 0x85, 0xa0, 0x01, 0x00, 0x24, 0xb1, 0x24, 0x20,
    0x40, 0x02, 0x41, 0x02, 0xb0, 0x15, 0xff, 0xfe, 0xe5, 0xff, 0xff, 0xff, 0xe5, 0xff, 0xff, 0xff,
    0xe5, 0xff, 0xff, 0xff, 0xe5, 0xff, 0xff, 0xff, 0xe5, 0xff, 0xff, 0xff, 0xe5, 0xff, 0xff, 0xff,
    0xe5, 0xff, 0x00, 0x34, 0xe5, 0xff, 0xff, 0xff, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00,
    0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00,
    0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00,
    0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00,
    0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0x70, 0x00, 0xd8, 0xc0, 0x01, 0x6e,
    0x88, 0xe8, 0x36, 0x00, 0xe1, 0x80, 0x56, 0x54, 0x1e, 0xc1, 0xf0, 0x00, 0xe1, 0x80, 0x07, 0xe1,
    0x21, 0x91, 0xf0, 0x00, 0x81, 0xc1, 0x00, 0x01, 0xb0, 0x91, 0x00, 0x03, 0x48, 0xff, 0xf0, 0x00,
    0xb0, 0x98, 0xff, 0xf9, 0x60, 0x01, 0xf0, 0x00, 0xb0, 0x98, 0x00, 0x02, 0x60, 0x00, 0xf0, 0x00,
};

quint16 seedKeyWord(quint16 seedWord) {
    return quint16(quint32(seedWord) * 135 + 1542);
}

QByteArray seedKey(const QByteArray &seed) {
    Q_ASSERT(seed.size() == 4);
    quint16 pk1 = (quint8(seed.at(0)) << 8) | quint8(seed.at(1));
    quint16 pk2 = (quint8(seed.at(2)) << 8) | quint8(seed.at(3));
    quint16 sk1 = seedKeyWord(pk1);
    quint16 sk2 = seedKeyWord(pk2);
    QByteArray key;
    key.append(char((sk1 >> 8) & 0xFF));
    key.append(char(sk1 & 0xFF));
    key.append(char((sk2 >> 8) & 0xFF));
    key.append(char(sk2 & 0xFF));
    return key;
}

quint16 checksum(const QByteArray &data) {
    quint16 sum = 0;
    for (int i = 0; i < data.size(); ++i)
        sum += quint8(data.at(i));
    return sum;
}

QByteArray buildRequestDownloadFrame(quint32 start, quint32 size) {
    QByteArray f;
    f.append(char(kServiceRequestDownload));
    f.append(char((start >> 16) & 0xFF));
    f.append(char((start >> 8) & 0xFF));
    f.append(char(start & 0xFF));
    f.append(char(0x00));
    f.append(char((size >> 16) & 0xFF));
    f.append(char((size >> 8) & 0xFF));
    f.append(char(size & 0xFF));
    return f;
}

QVector<QByteArray> buildTransferDataFrames(const QByteArray &payload) {
    QVector<QByteArray> frames;
    for (int offset = 0; offset < payload.size(); offset += int(kTransferChunkSize)) {
        QByteArray f;
        f.append(char(kServiceTransferData));
        f.append(payload.mid(offset, int(kTransferChunkSize)));
        frames.append(f);
    }
    return frames;
}

QByteArray buildRoutineCheckCrcFrame(quint32 targetStart) {
    QByteArray f;
    f.append(char(kServiceRoutineControl));
    f.append(char(kRoutineCheckCrc));
    f.append(char(targetStart < 0x800000 ? 2 : 1));
    return f;
}

QByteArray buildRoutineEraseFrame() {
    QByteArray f;
    f.append(char(kServiceRoutineControl));
    f.append(char(kRoutineErase));
    return f;
}

QByteArray buildRequestReflashUnlockFrame() {
    // Verbatim from externals/livemonitor/obdsessionwidget.cpp:180-181.
    // Original author's comment: "caused bootloader lockup". See header doc.
    static const quint8 kData[12] = {
        kServiceRequestReflash, 154, 1, 1, 'R', 'c', 'u', 's', '0', '0', 0, 1
    };
    return QByteArray(reinterpret_cast<const char *>(kData), sizeof(kData));
}

QByteArray buildReadMemoryByAddressFrame(quint32 addr, quint8 len) {
    QByteArray f;
    f.append(char(kServiceReadMemoryByAddress));
    f.append(char((addr >> 16) & 0xFF));
    f.append(char((addr >> 8) & 0xFF));
    f.append(char(addr & 0xFF));
    f.append(char(len));
    return f;
}

QByteArray buildDiagnosticSessionFrame(quint8 sessionId) {
    QByteArray f;
    f.append(char(kServiceDiagnosticSession));
    f.append(char(sessionId));
    return f;
}

QByteArray buildSecurityAccessSeedRequestFrame() {
    QByteArray f;
    f.append(char(kServiceSecurityAccess));
    f.append(char(0x05));
    return f;
}

QByteArray buildSecurityAccessKeyFrame(const QByteArray &key) {
    Q_ASSERT(key.size() == 4);
    QByteArray f;
    f.append(char(kServiceSecurityAccess));
    f.append(char(0x06));
    f.append(key);
    return f;
}

} // namespace MitsuColtCan
```

- [ ] **Step 6: Run the test build to verify it passes**

Run:
```bash
cd externals/FastECU/tests && qmake tests.pro && make 2>&1 | tail -30 && ./mut_dma_tests
```
Expected: build succeeds, all `TestMitsuColtCanProtocol` slots PASS (along with the pre-existing test suites).

- [ ] **Step 7: Commit**

```bash
cd externals/FastECU
git add protocol/mitsu_colt_can_protocol.h protocol/mitsu_colt_can_protocol.cpp \
        tests/test_mitsu_colt_can_protocol.h tests/test_mitsu_colt_can_protocol.cpp \
        tests/tests.pro tests/main.cpp
git commit -m "feat: add Mitsubishi Colt CZT CAN reflash protocol library

Pure, unit-tested frame builders for the UDS-style CAN reflash
protocol ported from externals/livemonitor/obdengine.cpp, targeting
ROM 47110032 (Colt CZT Z37A). Includes the erase/write-page RAM
routine blobs from colt_flasher.xml, carried over verbatim."
```

---

### Task 2: Flash device table entry for the Colt CZT MCU

**Files:**
- Modify: `externals/FastECU/kernelmemorymodels.h`

**Interfaces:**
- Consumes: nothing new.
- Produces: `flashdevices[]` entry named `"M32R_384KB_1block"`, looked up by `ecuCalDef->McuType` string in Task 3's module — used as `flashdevices[mcu_type_index].romsize` (full-chip read length) and `flashdevices[mcu_type_index].fblocks[0].start` (read start address, `0`).

- [ ] **Step 1: Add the enum value**

In `externals/FastECU/kernelmemorymodels.h`, in the `enum mcu_type` (around line 6), add a new value after `M32R_512KB_4blocks`:

```diff
     M32R_512KB,
     M32R_512KB_1block,
     M32R_512KB_4blocks,
+    M32R_384KB_1block,
     MC68HC16Y5,
```

- [ ] **Step 2: Add the flashblock/ramblock/kernelblock/eepromblock tables**

Immediately after the existing `fblocks_M32R_512KB_4blocks[]` block (around line 443, right before `const struct ramblock rblocks_M32R_512KB[]`), add:

```cpp
// Mitsubishi Colt CZT (Z37A, ROM 47110032): 384KB chip, single block
// covering the whole image for full-chip reads. Writes only ever target
// the 0x8000-0x60000 "userspace" subrange (see
// MitsuColtCan::kUserspaceStart/kUserspaceEnd in protocol/mitsu_colt_can_protocol.h);
// the protected 0x0-0x8000 boot region is never written. rblocks/kblocks/eblocks
// below are placeholders (not consulted by flash_ecu_mitsu_m32r_can.cpp) — same
// convention used for SH72531/N83M entries further down this table.
const struct flashblock fblocks_M32R_384KB_1block[] = {
    {0x00000000,    0x00060000},
};
```

- [ ] **Step 3: Add the table row**

In the `flashdevices[]` array (around line 521, right after the `"M32R_512KB_4blocks"` row), add:

```diff
     { "M32R_512KB_4blocks", M32R_512KB_4blocks, 512 * 1024, 4, fblocks_M32R_512KB_4blocks, rblocks_M32R_512KB, kblocks_M32R_512KB, eblocks_M32R_512KB },
+    { "M32R_384KB_1block", M32R_384KB_1block, 384 * 1024, 1, fblocks_M32R_384KB_1block, rblocks_M32R_512KB, kblocks_M32R_512KB, eblocks_M32R_512KB },
     { "MC68HC16Y5", MC68HC16Y5, 160 * 1024, 10, fblocks_MC68HC16Y5, rblocks_MC68HC16Y5, kblocks_MC68HC16Y5, eblocks_MC68HC16Y5 },
```

- [ ] **Step 4: Verify it compiles**

Run:
```bash
cd externals/FastECU && qmake FastECU.pro && make 2>&1 | tail -30
```
Expected: build succeeds (this header is included project-wide; a syntax error here would fail the whole build).

- [ ] **Step 5: Commit**

```bash
cd externals/FastECU
git add kernelmemorymodels.h
git commit -m "feat: add M32R_384KB_1block flashdev entry for Colt CZT (Z37A)"
```

---

### Task 3: New ECU module scaffolding, wired end-to-end with stub bodies

**Files:**
- Create: `externals/FastECU/modules/ecu/flash_ecu_mitsu_m32r_can.h`
- Create: `externals/FastECU/modules/ecu/flash_ecu_mitsu_m32r_can.cpp`
- Modify: `externals/FastECU/FastECU.pro`
- Modify: `externals/FastECU/mainwindow.h`
- Modify: `externals/FastECU/mainwindow.cpp`
- Modify: `externals/FastECU/config/protocols.cfg`

**Interfaces:**
- Consumes: `MitsuColtCan::*` from Task 1, `flashdevices[]` `"M32R_384KB_1block"` entry from Task 2.
- Produces: class `FlashEcuMitsuM32rCan : public QDialog` with public `void run()`, matching the existing module convention (`connect_signals_and_run_module(FLASH_CLASS*)` in `mainwindow.h:331` requires the `external_logger(QString)`, `external_logger(int)`, `LOG_E/LOG_W/LOG_I/LOG_D(QString,bool,bool)` signals and a constructor `(SerialPortActions*, FileActions::EcuCalDefStructure*, QString, QWidget*)`). Private methods `connect_bootloader()`, `read_mem(uint32_t,uint32_t)`, `write_mem(bool)` are stubbed to return `STATUS_ERROR` in this task and implemented in Tasks 4-6.

- [ ] **Step 1: Create the module header**

Create `externals/FastECU/modules/ecu/flash_ecu_mitsu_m32r_can.h`:

```cpp
#ifndef FLASH_ECU_MITSU_M32R_CAN_H
#define FLASH_ECU_MITSU_M32R_CAN_H

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QMainWindow>
#include <QMessageBox>
#include <QSerialPort>
#include <QTime>
#include <QTimer>
#include <QWidget>

#include <kernelmemorymodels.h>
#include <file_actions.h>
#include <ui_ecu_operations.h>

//Forward declaration
class SerialPortActions;

QT_BEGIN_NAMESPACE
namespace Ui
{
    class EcuOperationsWindow;
}
QT_END_NAMESPACE

// Mitsubishi Colt CZT (Z37A, ROM 47110032) CAN reflash module. Protocol
// logic lives in protocol/mitsu_colt_can_protocol.h; this class is the
// orchestration layer (session handling, I/O, UI) around it.
class FlashEcuMitsuM32rCan : public QDialog
{
    Q_OBJECT

public:
    explicit FlashEcuMitsuM32rCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent = nullptr);
    ~FlashEcuMitsuM32rCan();

    void run();

signals:
    void external_logger(QString message);
    void external_logger(int value);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

private:
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;

    #define STATUS_SUCCESS  0x00
    #define STATUS_ERROR    0x01

    bool kill_process = false;
    int mcu_type_index;

    uint16_t serial_read_timeout = 500;
    uint16_t serial_read_extra_long_timeout = 3000;

    QString mcu_type_string;

    void closeEvent(QCloseEvent *event);

    int connect_bootloader();
    int read_mem(uint32_t start_addr, uint32_t length);
    int write_mem(bool test_write);
    bool upload_and_commit(uint32_t start, const QByteArray &data);

    QByteArray build_request(const QByteArray &sidPayload);
    QString parse_message_to_hex(QByteArray received);
    void set_progressbar_value(int value);
    void delay(int timeout);

    SerialPortActions *serial;
    Ui::EcuOperationsWindow *ui;
};

#endif // FLASH_ECU_MITSU_M32R_CAN_H
```

- [ ] **Step 2: Create the module implementation with stub protocol methods**

Create `externals/FastECU/modules/ecu/flash_ecu_mitsu_m32r_can.cpp`:

```cpp
#include "flash_ecu_mitsu_m32r_can.h"
#include "serial_port_actions.h"
#include "protocol/mitsu_colt_can_protocol.h"

FlashEcuMitsuM32rCan::FlashEcuMitsuM32rCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::EcuOperationsWindow)
    , ecuCalDef(ecuCalDef)
    , cmd_type(cmd_type)
{
    ui->setupUi(this);

    if (cmd_type == "write")
        this->setWindowTitle("Write ROM " + ecuCalDef->FileName + " to ECU");
    else if (cmd_type == "read")
        this->setWindowTitle("Read ROM from ECU");

    this->serial = serial;
}

FlashEcuMitsuM32rCan::~FlashEcuMitsuM32rCan()
{
    delete ui;
}

void FlashEcuMitsuM32rCan::closeEvent(QCloseEvent *event)
{
    kill_process = true;
}

void FlashEcuMitsuM32rCan::run()
{
    this->show();

    int result = STATUS_ERROR;
    set_progressbar_value(0);

    mcu_type_string = ecuCalDef->McuType;
    mcu_type_index = 0;
    while (flashdevices[mcu_type_index].name != 0)
    {
        if (flashdevices[mcu_type_index].name == mcu_type_string)
            break;
        mcu_type_index++;
    }
    emit LOG_D("MCU type: " + QString(flashdevices[mcu_type_index].name) + " index: " + QString::number(mcu_type_index), true, true);

    serial->set_is_iso14230_connection(false);
    serial->set_add_iso14230_header(false);
    serial->set_is_can_connection(false);
    serial->set_is_iso15765_connection(true);
    serial->set_is_29_bit_id(false);
    serial->set_can_speed("500000");
    serial->set_iso15765_source_address(0x7E0);
    serial->set_iso15765_destination_address(0x7E8);
    serial->open_serial_port();

    int ret = QMessageBox::warning(this, tr("Connecting to ECU"),
                                   tr("Turn ignition ON and press OK to start initializing connection to ECU"),
                                   QMessageBox::Ok | QMessageBox::Cancel,
                                   QMessageBox::Ok);

    switch (ret)
    {
        case QMessageBox::Ok:
            emit LOG_I("Connecting to Mitsubishi Colt CZT M32R CAN bootloader, please wait...", true, true);
            result = connect_bootloader();

            if (result == STATUS_SUCCESS)
            {
                if (cmd_type == "read")
                {
                    emit external_logger("Reading ROM, please wait...");
                    emit LOG_I("Reading ROM from ECU using CAN", true, true);
                    result = read_mem(flashdevices[mcu_type_index].fblocks[0].start, flashdevices[mcu_type_index].romsize);
                }
                else if (cmd_type == "write")
                {
                    emit external_logger("Writing ROM, please wait...");
                    emit LOG_I("Writing ROM to ECU using CAN", true, true);
                    result = write_mem(false);
                }
            }
            emit external_logger("Finished");

            if (result == STATUS_SUCCESS)
            {
                QMessageBox::information(this, tr("ECU Operation"), "ECU operation was succesful, press OK to exit");
                this->close();
            }
            else
            {
                QMessageBox::warning(this, tr("ECU Operation"), "ECU operation failed, press OK to exit and try again");
            }
            break;
        case QMessageBox::Cancel:
            emit LOG_D("Operation canceled", true, true);
            this->close();
            break;
        default:
            QMessageBox::warning(this, tr("Connecting to ECU"), "Unknown operation selected!");
            emit LOG_D("Unknown operation selected!", true, true);
            this->close();
            break;
    }
}

QByteArray FlashEcuMitsuM32rCan::build_request(const QByteArray &sidPayload)
{
    QByteArray output;
    output.append(char(0x00));
    output.append(char(0x00));
    output.append(char(0x07));
    output.append(char(0xE0));
    output.append(sidPayload);
    return output;
}

int FlashEcuMitsuM32rCan::connect_bootloader()
{
    emit LOG_E("connect_bootloader() not yet implemented", true, true);
    return STATUS_ERROR;
}

int FlashEcuMitsuM32rCan::read_mem(uint32_t start_addr, uint32_t length)
{
    Q_UNUSED(start_addr);
    Q_UNUSED(length);
    emit LOG_E("read_mem() not yet implemented", true, true);
    return STATUS_ERROR;
}

int FlashEcuMitsuM32rCan::write_mem(bool test_write)
{
    Q_UNUSED(test_write);
    emit LOG_E("write_mem() not yet implemented", true, true);
    return STATUS_ERROR;
}

bool FlashEcuMitsuM32rCan::upload_and_commit(uint32_t start, const QByteArray &data)
{
    Q_UNUSED(start);
    Q_UNUSED(data);
    return false;
}

QString FlashEcuMitsuM32rCan::parse_message_to_hex(QByteArray received)
{
    QString msg;
    for (int i = 0; i < received.length(); i++)
        msg.append(QString("%1 ").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUtf8());
    return msg;
}

void FlashEcuMitsuM32rCan::set_progressbar_value(int value)
{
    bool valueChanged = true;
    if (ui->progressbar)
    {
        valueChanged = ui->progressbar->value() != value;
        ui->progressbar->setValue(value);
    }
    if (valueChanged)
        emit external_logger(value);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

void FlashEcuMitsuM32rCan::delay(int timeout)
{
    QTime dieTime = QTime::currentTime().addMSecs(timeout);
    while (QTime::currentTime() < dieTime)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
}
```

- [ ] **Step 3: Add the new files to the qmake project**

In `externals/FastECU/FastECU.pro`, `SOURCES` (after line 134, the `flash_ecu_subaru_mitsu_m32r_kline.cpp` line):

```diff
     modules/ecu/flash_ecu_subaru_mitsu_m32r_kline.cpp \
+    modules/ecu/flash_ecu_mitsu_m32r_can.cpp \
     modules/ecu/flash_ecu_subaru_unisia_jecs.cpp \
```

`HEADERS` (after line 214, the `flash_ecu_subaru_mitsu_m32r_kline.h` line):

```diff
     modules/ecu/flash_ecu_subaru_mitsu_m32r_kline.h \
+    modules/ecu/flash_ecu_mitsu_m32r_can.h \
     modules/ecu/flash_ecu_subaru_unisia_jecs.h \
```

- [ ] **Step 4: Wire the dispatch in mainwindow**

In `externals/FastECU/mainwindow.h`, add the include after line 75 (`flash_ecu_subaru_mitsu_m32r_kline.h`):

```diff
 #include <modules/ecu/flash_ecu_subaru_mitsu_m32r_kline.h>
+#include <modules/ecu/flash_ecu_mitsu_m32r_can.h>
 #include <modules/ecu/flash_ecu_subaru_hitachi_sh7058_can.h>
```

In `externals/FastECU/mainwindow.cpp`, add a dispatch branch after the `sub_ecu_mitsu_m32r_kline` block (after line 1327):

```diff
         else if (configValues->flash_protocol_selected_protocol_name.startsWith("sub_ecu_mitsu_m32r_kline"))
         {
             FlashEcuSubaruMitsuM32rKline flash_module(serial, ecuCalDef[rom_number], cmd_type, this);
             connect_signals_and_run_module(&flash_module);
         }
+        else if (configValues->flash_protocol_selected_protocol_name.startsWith("mitsu_ecu_m32r_can"))
+        {
+            FlashEcuMitsuM32rCan flash_module(serial, ecuCalDef[rom_number], cmd_type, this);
+            connect_signals_and_run_module(&flash_module);
+        }
         else if (configValues->flash_protocol_selected_protocol_name.startsWith("sub_ecu_hitachi_sh7058_can"))
```

- [ ] **Step 5: Add the protocol and vehicle entries**

In `externals/FastECU/config/protocols.cfg`, add a new `<protocol>` block after the `mitsu_ecu_m32r_kline_mut_dma` block (after line 786):

```diff
             <description>Mitsubishi M32R K-Line MUT/DMA free-form logging (experimental)</description>
         </protocol>
+        <protocol name="mitsu_ecu_m32r_can">
+            <ecu>Mitsubishi M32R</ecu>
+            <mcu>M32R_384KB_1block</mcu>
+            <mode>OBD2</mode>
+            <checksum>no</checksum>
+            <read>yes</read>
+            <test_write>no</test_write>
+            <write>yes</write>
+            <flash_transport>iso15765</flash_transport>
+            <log_transport>K-Line</log_transport>
+            <log_protocol>SSM</log_protocol>
+            <cal_id_ascii>no</cal_id_ascii>
+            <cal_id_addr>0x0</cal_id_addr>
+            <cal_id_length>0</cal_id_length>
+            <description>Mitsubishi Colt CZT Z37A CAN (M32176F3/384KB, ROM 47110032)</description>
+        </protocol>
         <protocol name="sub_ecu_hitachi_sh7058_can">
```

Add a new `<car_model>` block after the `sub_ecu_mitsu_m32r_kline` car_model (after line 1646):

```diff
             <year>Unk</year>
             <protocol>sub_ecu_mitsu_m32r_kline</protocol>
         </car_model>
+        <car_model>
+            <make>Mitsubishi</make>
+            <model>Colt CZT</model>
+            <version>Z37A 5MT</version>
+            <type>4G15T</type>
+            <kw>110</kw>
+            <hp>150</hp>
+            <fuel>Petrol</fuel>
+            <year>2005</year>
+            <protocol>mitsu_ecu_m32r_can</protocol>
+        </car_model>
         <car_model>
             <make>Subaru</make>
             <model>Unknown</model>
```

- [ ] **Step 6: Verify the full project builds**

Run:
```bash
cd externals/FastECU && qmake FastECU.pro && make 2>&1 | tail -40
```
Expected: build succeeds with no errors (warnings from pre-existing files are fine; the new module and its callers must compile and link cleanly).

- [ ] **Step 7: Commit**

```bash
cd externals/FastECU
git add modules/ecu/flash_ecu_mitsu_m32r_can.h modules/ecu/flash_ecu_mitsu_m32r_can.cpp \
        FastECU.pro mainwindow.h mainwindow.cpp config/protocols.cfg
git commit -m "feat: scaffold Mitsubishi Colt CZT CAN flash module and wire into FastECU

New FlashEcuMitsuM32rCan module is wired end-to-end (config protocol
entry, Mitsubishi Colt CZT vehicle entry, mainwindow dispatch) with
connect_bootloader/read_mem/write_mem stubbed pending Tasks 4-6."
```

---

### Task 4: Implement `connect_bootloader()`

**Files:**
- Modify: `externals/FastECU/modules/ecu/flash_ecu_mitsu_m32r_can.cpp:103-107` (the stub from Task 3)

**Interfaces:**
- Consumes: `MitsuColtCan::kSessionBasic/kSessionBootload`, `buildDiagnosticSessionFrame`, `buildSecurityAccessSeedRequestFrame`, `buildSecurityAccessKeyFrame`, `seedKey` (Task 1); `build_request`, `parse_message_to_hex`, `serial_read_timeout` (Task 3).
- Produces: `connect_bootloader()` returning `STATUS_SUCCESS`/`STATUS_ERROR`, called from `run()` (Task 3).

- [ ] **Step 1: Replace the stub with the real implementation**

In `externals/FastECU/modules/ecu/flash_ecu_mitsu_m32r_can.cpp`, replace:

```cpp
int FlashEcuMitsuM32rCan::connect_bootloader()
{
    emit LOG_E("connect_bootloader() not yet implemented", true, true);
    return STATUS_ERROR;
}
```

with:

```cpp
/*
 * Connect to the Mitsubishi Colt CZT (Z37A) M32R CAN bootloader.
 * Read only needs the basic diagnostic session; write needs the bootload
 * session plus security access (matches
 * externals/livemonitor/obdsessionwidget.cpp's session-id selection and
 * obdengine.cpp's ObdSessionInitCommandSequence).
 *
 * @return success
 */
int FlashEcuMitsuM32rCan::connect_bootloader()
{
    using namespace MitsuColtCan;

    QByteArray output;
    QByteArray received;
    QString msg;

    if (!serial->is_serial_port_open())
    {
        emit LOG_I("ERROR: Serial port is not open.", true, true);
        return STATUS_ERROR;
    }

    bool needSecurity = (cmd_type == "write");
    quint8 sessionId = needSecurity ? kSessionBootload : kSessionBasic;

    emit LOG_I("Starting diagnostic session...", true, true);
    output = build_request(buildDiagnosticSessionFrame(sessionId));
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 5 || (uint8_t)received.at(4) != (kServiceDiagnosticSession + 0x40) || (uint8_t)received.at(5) != sessionId)
    {
        emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Diagnostic session ok", true, true);

    if (!needSecurity)
        return STATUS_SUCCESS;

    emit LOG_I("Requesting security seed...", true, true);
    output = build_request(buildSecurityAccessSeedRequestFrame());
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 9 || (uint8_t)received.at(4) != (kServiceSecurityAccess + 0x40) || (uint8_t)received.at(5) != 5)
    {
        emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }

    QByteArray seed = received.mid(6, 4);
    msg = parse_message_to_hex(seed);
    emit LOG_I("Received seed: " + msg, true, true);

    QByteArray key = seedKey(seed);
    msg = parse_message_to_hex(key);
    emit LOG_I("Calculated seed key: " + msg, true, true);

    emit LOG_I("Sending seed key to ECU...", true, true);
    output = build_request(buildSecurityAccessKeyFrame(key));
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 5 || (uint8_t)received.at(4) != (kServiceSecurityAccess + 0x40) || (uint8_t)received.at(5) != 6)
    {
        emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Security access ok", true, true);

    return STATUS_SUCCESS;
}
```

- [ ] **Step 2: Verify it compiles**

Run:
```bash
cd externals/FastECU && qmake FastECU.pro && make 2>&1 | tail -30
```
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
cd externals/FastECU
git add modules/ecu/flash_ecu_mitsu_m32r_can.cpp
git commit -m "feat: implement connect_bootloader() for Colt CZT CAN module

Diagnostic session (basic for read, bootload for write) plus security
access with the Mitsubishi-specific seed-key algorithm, ported from
externals/livemonitor/obdengine.cpp's ObdSessionInitCommandSequence."
```

No automated test is possible here without real hardware (it's a blocking I/O sequence against a live ECU). Verification is bench-only, per Task 7's checklist.

---

### Task 5: Implement `read_mem()`

**Files:**
- Modify: `externals/FastECU/modules/ecu/flash_ecu_mitsu_m32r_can.cpp` (the stub from Task 3)

**Interfaces:**
- Consumes: `MitsuColtCan::kServiceReadMemoryByAddress`, `kFlashReadBlockSize`, `buildReadMemoryByAddressFrame` (Task 1); `build_request`, `set_progressbar_value` (Task 3).
- Produces: `read_mem(uint32_t, uint32_t)` populating `ecuCalDef->FullRomData`, called from `run()` (Task 3).

- [ ] **Step 1: Replace the stub with the real implementation**

In `externals/FastECU/modules/ecu/flash_ecu_mitsu_m32r_can.cpp`, replace:

```cpp
int FlashEcuMitsuM32rCan::read_mem(uint32_t start_addr, uint32_t length)
{
    Q_UNUSED(start_addr);
    Q_UNUSED(length);
    emit LOG_E("read_mem() not yet implemented", true, true);
    return STATUS_ERROR;
}
```

with:

```cpp
/*
 * Read ROM from the Colt CZT bootloader via ReadMemoryByAddress(0x23),
 * kFlashReadBlockSize-byte blocks. No security session required for read
 * (matches externals/livemonitor/obdsessionwidget.cpp's requestReadFlashBlock,
 * which only needs ObdEngine::SessionBasic).
 *
 * @return success
 */
int FlashEcuMitsuM32rCan::read_mem(uint32_t start_addr, uint32_t length)
{
    using namespace MitsuColtCan;

    QByteArray output;
    QByteArray received;
    QByteArray romdata;

    uint32_t addr = start_addr;
    uint32_t end_addr = start_addr + length;

    set_progressbar_value(0);
    emit LOG_I("Start reading ROM, please wait...", true, true);

    while (addr < end_addr)
    {
        if (kill_process)
            return STATUS_ERROR;

        output = build_request(buildReadMemoryByAddressFrame(addr, (quint8)kFlashReadBlockSize));
        serial->write_serial_data_echo_check(output);
        delay(50);
        received = serial->read_serial_data(serial_read_timeout);

        if (received.length() < int(4 + 1 + kFlashReadBlockSize) || (uint8_t)received.at(4) != (kServiceReadMemoryByAddress + 0x40))
        {
            emit LOG_E("Wrong response from ECU at 0x" + QString::number(addr, 16) + ": " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
            return STATUS_ERROR;
        }

        romdata.append(received.mid(5, int(kFlashReadBlockSize)));
        addr += kFlashReadBlockSize;

        float pleft = (float)(addr - start_addr) / (float)length * 100.0f;
        set_progressbar_value((int)pleft);
    }

    emit LOG_I("ROM read complete", true, true);
    ecuCalDef->FullRomData = romdata;
    set_progressbar_value(100);

    return STATUS_SUCCESS;
}
```

- [ ] **Step 2: Verify it compiles**

Run:
```bash
cd externals/FastECU && qmake FastECU.pro && make 2>&1 | tail -30
```
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
cd externals/FastECU
git add modules/ecu/flash_ecu_mitsu_m32r_can.cpp
git commit -m "feat: implement read_mem() for Colt CZT CAN module

192-byte ReadMemoryByAddress block loop over the full 384KB image,
ported from externals/livemonitor/obdsessionwidget.cpp's
requestReadFlashBlock."
```

No automated test is possible here without real hardware. Verification is bench-only, per Task 7's checklist.

---

### Task 6: Implement `upload_and_commit()` and `write_mem()`

**Files:**
- Modify: `externals/FastECU/modules/ecu/flash_ecu_mitsu_m32r_can.cpp` (the stubs from Task 3)

**Interfaces:**
- Consumes: `MitsuColtCan::buildRequestDownloadFrame`, `buildTransferDataFrames`, `checksum`, `buildRoutineCheckCrcFrame`, `buildRoutineEraseFrame`, `buildRequestReflashUnlockFrame`, `kCrcTransferAddress`, `kCrcTransferSize`, `kEraseRoutineRamAddr`, `kWriteRoutineRamAddr`, `kErasePageRoutine`, `kWritePageRoutine`, `kUserspaceStart`, `kUserspaceEnd` (Task 1); `build_request` (Task 3).
- Produces: `write_mem(bool)`, called from `run()` (Task 3).

- [ ] **Step 1: Replace the `upload_and_commit` stub**

Replace:

```cpp
bool FlashEcuMitsuM32rCan::upload_and_commit(uint32_t start, const QByteArray &data)
{
    Q_UNUSED(start);
    Q_UNUSED(data);
    return false;
}
```

with:

```cpp
/*
 * RequestDownload + chunked TransferData of `data` to `start`, followed by
 * a RequestDownload + TransferData of its checksum to the fixed
 * kCrcTransferAddress, then RoutineControl(225) to make the ECU verify and
 * commit the transfer. Mirrors ObdFlashWriteCommandSequence in
 * externals/livemonitor/obdengine.cpp exactly — every upload in that
 * protocol (the erase-page blob, the write-page blob, and the final ROM
 * data) goes through this same five-step sequence.
 *
 * @return success
 */
bool FlashEcuMitsuM32rCan::upload_and_commit(uint32_t start, const QByteArray &data)
{
    using namespace MitsuColtCan;

    QByteArray output;
    QByteArray received;

    output = build_request(buildRequestDownloadFrame(start, data.size()));
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRequestDownload + 0x40))
    {
        emit LOG_E("RequestDownload to 0x" + QString::number(start, 16) + " rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    const QVector<QByteArray> chunks = buildTransferDataFrames(data);
    for (const QByteArray &chunk : chunks)
    {
        output = build_request(chunk);
        serial->write_serial_data_echo_check(output);
        delay(50);
        received = serial->read_serial_data(serial_read_timeout);
        if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceTransferData + 0x40))
        {
            emit LOG_E("TransferData to 0x" + QString::number(start, 16) + " rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
            return false;
        }
    }

    output = build_request(buildRequestDownloadFrame(kCrcTransferAddress, kCrcTransferSize));
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRequestDownload + 0x40))
    {
        emit LOG_E("RequestDownload for checksum rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    quint16 crc = checksum(data);
    QByteArray crcPayload;
    crcPayload.append(char(kServiceTransferData));
    crcPayload.append(char((crc >> 8) & 0xFF));
    crcPayload.append(char(crc & 0xFF));
    output = build_request(crcPayload);
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceTransferData + 0x40))
    {
        emit LOG_E("TransferData for checksum rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    output = build_request(buildRoutineCheckCrcFrame(start));
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_extra_long_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRoutineControl + 0x40))
    {
        emit LOG_E("RoutineControl CRC check for 0x" + QString::number(start, 16) + " rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    return true;
}
```

- [ ] **Step 2: Replace the `write_mem` stub**

Replace:

```cpp
int FlashEcuMitsuM32rCan::write_mem(bool test_write)
{
    Q_UNUSED(test_write);
    emit LOG_E("write_mem() not yet implemented", true, true);
    return STATUS_ERROR;
}
```

with:

```cpp
/*
 * Write the patched ROM's userspace region (0x8000-0x60000) to the Colt
 * CZT bootloader. Four stages, ported from
 * externals/livemonitor/obdsessionwidget.cpp's
 * erasePageUploadedCallback/writePageUploadedCallback/flashErasedCallback
 * chain: upload erase-page routine, upload write-page routine, trigger
 * erase, upload ROM data.
 *
 * test_write is not implemented for this protocol (matches every other
 * M32R module in FastECU; see config/protocols.cfg's test_write=no).
 *
 * @return success
 */
int FlashEcuMitsuM32rCan::write_mem(bool test_write)
{
    using namespace MitsuColtCan;
    Q_UNUSED(test_write);

    QByteArray romdata = ecuCalDef->FullRomData;
    if ((uint32_t)romdata.size() < kUserspaceEnd)
    {
        emit LOG_E("ROM file too small: need at least 0x" + QString::number(kUserspaceEnd, 16) + " bytes", true, true);
        return STATUS_ERROR;
    }

    emit LOG_I("Uploading erase-page routine to RAM 0x" + QString::number(kEraseRoutineRamAddr, 16) + "...", true, true);
    if (!upload_and_commit(kEraseRoutineRamAddr, QByteArray(reinterpret_cast<const char *>(kErasePageRoutine), sizeof(kErasePageRoutine))))
    {
        emit LOG_E("Erase-page routine upload failed", true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Erase page uploaded", true, true);

    emit LOG_I("Uploading write-page routine to RAM 0x" + QString::number(kWriteRoutineRamAddr, 16) + "...", true, true);
    if (!upload_and_commit(kWriteRoutineRamAddr, QByteArray(reinterpret_cast<const char *>(kWritePageRoutine), sizeof(kWritePageRoutine))))
    {
        emit LOG_E("Write-page routine upload failed", true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Write page uploaded", true, true);

    // --- HIGH RISK STEP ---
    // This 12-byte ServiceRequestReflash(0x3B) payload is carried over verbatim
    // from externals/livemonitor/obdsessionwidget.cpp:180-181, where the
    // original author's own comment reads "caused bootloader lockup" during
    // their testing. Only attempt this on a bench/spare ECU with a recovery
    // path available (see this project's boot-talk utility for bricked-ECU
    // recovery). See docs/superpowers/specs/2026-06-30-fastecu-colt-can-reflash-design.md
    // for full context.
    int reflashConfirm = QMessageBox::warning(this, tr("Erase trigger"),
        tr("About to send the flash-erase trigger command. This exact "
           "sequence is known to have locked up the bootloader during the "
           "original implementation's testing. Only continue if this is a "
           "bench/spare ECU with a recovery path available.\n\nContinue?"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (reflashConfirm != QMessageBox::Yes)
    {
        emit LOG_I("Erase trigger canceled by user", true, true);
        return STATUS_ERROR;
    }

    QByteArray output = build_request(buildRequestReflashUnlockFrame());
    serial->write_serial_data_echo_check(output);
    delay(200);
    QByteArray received = serial->read_serial_data(serial_read_extra_long_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRequestReflash + 0x40))
    {
        emit LOG_E("Reflash unlock rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }

    output = build_request(buildRoutineEraseFrame());
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_extra_long_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRoutineControl + 0x40))
    {
        emit LOG_E("Erase trigger rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Userspace flash erased", true, true);

    emit LOG_I("Writing ROM userspace 0x" + QString::number(kUserspaceStart, 16) + "-0x" + QString::number(kUserspaceEnd, 16) + "...", true, true);
    QByteArray userspace = romdata.mid(int(kUserspaceStart), int(kUserspaceEnd - kUserspaceStart));
    if (!upload_and_commit(kUserspaceStart, userspace))
    {
        emit LOG_E("ROM userspace write failed", true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Userspace flash written", true, true);

    return STATUS_SUCCESS;
}
```

- [ ] **Step 3: Verify it compiles**

Run:
```bash
cd externals/FastECU && qmake FastECU.pro && make 2>&1 | tail -30
```
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
cd externals/FastECU
git add modules/ecu/flash_ecu_mitsu_m32r_can.cpp
git commit -m "feat: implement write_mem() for Colt CZT CAN module

Erase-page upload, write-page upload, flagged erase-trigger (gated
behind an explicit confirmation dialog due to a known bootloader-
lockup risk in the source material), then the ROM userspace write.
Ported from externals/livemonitor/obdsessionwidget.cpp's upload
callback chain."
```

No automated test is possible here without real hardware. Verification is bench-only, per Task 7's checklist — and this task in particular must not be exercised against a running vehicle.

---

### Task 7: Bench-test checklist and final verification

**Files:**
- Create: `externals/FastECU/docs/colt_czt_47110032_can_bench_checklist.md`

**Interfaces:**
- Consumes: nothing (documentation only).
- Produces: nothing consumed by other tasks; this is the bench-qualification gate referenced by the spec's Safety/risk section.

- [ ] **Step 1: Write the bench checklist**

Create `externals/FastECU/docs/colt_czt_47110032_can_bench_checklist.md`:

```markdown
# Colt CZT (Z37A, 47110032) CAN reflash — bench qualification checklist

Gate before any real-vehicle use of `FlashEcuMitsuM32rCan` (protocol
`mitsu_ecu_m32r_can`). Same convention as this project's other
bench-qualification gates (e.g. `mmc-patches/m32r/39670016_z27a_mt_audm/mode23-bench-notes.md`).
Do not skip steps. Have a recovery path (this project's `boot-talk`
utility) available before Step 4.

1. **Connect.** Power a bench/spare Z37A ECU, select "Mitsubishi / Colt CZT
   / Z37A 5MT" in FastECU, choose Read. Confirm `connect_bootloader()`
   completes (diagnostic session ok) without security access (read uses
   the basic session).
2. **Full read.** Read the full 384KB image. Compare byte-for-byte against
   a known-good reference dump of the same ROM (`47110032`, per
   `mmc-definitions/roms/m32r/47110032/rom.toml`).
3. **Write-path dry run on the bench ECU (not a vehicle).** Select Write
   with a known-good ROM file. Confirm the erase-page and write-page
   routine uploads each complete (`upload_and_commit` returns true for
   both `0x805568` and `0x8054AC`).
4. **Erase trigger — highest risk step.** Confirm the in-app warning
   dialog appears before the `ServiceRequestReflash`(0x3B) unlock frame is
   sent. Only proceed past it with the bench ECU connected and a recovery
   path on hand. If the bootloader locks up (matching the original
   author's experience in `externals/livemonitor`), use `boot-talk` to
   recover before continuing.
5. **Full write + read-back.** Once the erase trigger succeeds, confirm
   the userspace write (`0x8000`-`0x60000`) completes, then re-read the
   full ROM and diff against the file that was written.
6. **Only after 1-5 pass repeatably on a bench/spare ECU**, consider use
   on a real vehicle.
```

- [ ] **Step 2: Run the full build and test suite one more time**

Run:
```bash
cd externals/FastECU && qmake FastECU.pro && make 2>&1 | tail -40
cd tests && qmake tests.pro && make 2>&1 | tail -30 && ./mut_dma_tests
```
Expected: both builds succeed; all tests pass, including the new `TestMitsuColtCanProtocol` suite from Task 1.

- [ ] **Step 3: Commit**

```bash
cd externals/FastECU
git add docs/colt_czt_47110032_can_bench_checklist.md
git commit -m "docs: bench qualification checklist for Colt CZT CAN reflash"
```
