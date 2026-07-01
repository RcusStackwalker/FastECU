# FastECU Cdbg CAN Logging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port livemonitor's raw-CAN "Cdbg" live-data logging protocol into FastECU as a third `log_protocol` value (`"CDBG"`, alongside `"SSM"` and `"MUT_DMA"`), giving the Colt CZT (Z37A, ROM `47110032`) a live-dashboard logging path over its existing CAN reflash connection.

**Architecture:** A pure protocol codec (`protocol/mitsu_colt_can_cdbg_protocol.{h,cpp}`) builds/parses the 8-byte Cdbg command and reply frames with no I/O. A small raw-CAN transport abstraction (`protocol/ican_transport.h` + `protocol/fastecu_can_transport.{h,cpp}`) adapts FastECU's existing `SerialPortActions` raw-CAN mode. A driver (`protocol/mitsu_colt_can_cdbg_driver.{h,cpp}`) orchestrates the session/security handshake, frame configuration, and streaming poll against that transport. `log_operations_mitsubishi_cdbg.cpp` wires the driver into `MainWindow` exactly the way `log_operations_mitsubishi.cpp` already wires in `MutDmaDriver` for MUT/DMA — same shared `logparams_poll_timer`, same `logValues` channel-list convention, same `menu_actions.cpp::toggle_realtime()` dispatch point.

**Tech Stack:** C++17, Qt 6 (Core, Test), qmake6, QtTest.

## Global Constraints

- Target ECU is `47110032` (Mitsubishi Colt CZT, Z37A) only. Do not generalize to other ROMs in this plan.
- Cdbg is raw CAN, not ISO-TP: request CAN ID `0x630`, reply CAN ID `0x631`, every frame is exactly 8 bytes, no multi-frame segmentation.
- Do not port livemonitor's `schema.xml`/`czt_schema.xml` RAM pointer values as real channel addresses — they are unverified for `47110032`. Any example/placeholder logger definition must use obviously-fake addresses, matching `config/logger_mut_dma_example.xml`'s own convention.
- Follow this codebase's established protocol-library style (see `protocol/mitsu_colt_can_protocol.cpp`): build frames with sequential `QByteArray::append(char(...))` calls and manual bit-shifts, not `QtEndian` helpers. Test expected values with `QByteArray::fromHex(...)`.
- No CI coverage is possible for anything that touches real hardware (the transport adapter, the `MainWindow` integration). Only the pure codec and the driver (via a scripted transport test double) get automated tests, matching the existing `MutDmaDriver`/`ScriptedKlineTransport` precedent.
- Reference spec: `docs/superpowers/specs/2026-07-01-fastecu-cdbg-can-logging-design.md`.

---

### Task 1: Cdbg protocol codec — session & security frames

**Files:**
- Create: `protocol/mitsu_colt_can_cdbg_protocol.h`
- Create: `protocol/mitsu_colt_can_cdbg_protocol.cpp`
- Create: `tests/test_mitsu_colt_can_cdbg_protocol.h`
- Create: `tests/test_mitsu_colt_can_cdbg_protocol.cpp`
- Modify: `tests/tests.pro`
- Modify: `tests/main.cpp`

**Interfaces:**
- Produces: `namespace MitsuColtCanCdbg`: `kRequestCanId` (`quint32`, `0x630`), `kReplyCanId` (`quint32`, `0x631`), `struct CdbgChannel { quint32 pointer; quint8 size; }`, `QByteArray buildInitFrame()`, `QByteArray buildSecuritySeedRequestFrame()`, `quint32 seedToKey(quint32 seed)`, `quint32 extractSeed(const QByteArray &reply)`, `QByteArray buildSecurityKeyFrame(quint32 key)`, `bool securityGranted(const QByteArray &reply)`, `QByteArray buildLogResetFrame(quint8 instance)`, `QByteArray buildLogStartFrame(quint8 instance, quint8 frameCount, quint32 intervalMs)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_mitsu_colt_can_cdbg_protocol.h`:

```cpp
#pragma once
int run_test_mitsu_colt_can_cdbg_protocol(int argc, char** argv);
```

Create `tests/test_mitsu_colt_can_cdbg_protocol.cpp`:

```cpp
#include <QtTest>
#include "protocol/mitsu_colt_can_cdbg_protocol.h"
#include "test_mitsu_colt_can_cdbg_protocol.h"
using namespace MitsuColtCanCdbg;

class TestMitsuColtCanCdbgProtocol : public QObject { Q_OBJECT
private slots:
    void init_frame_layout() {
        QCOMPARE(buildInitFrame(), QByteArray::fromHex("0101000000000000"));
    }
    void security_seed_request_frame_layout() {
        QCOMPARE(buildSecuritySeedRequestFrame(), QByteArray::fromHex("1200020000000000"));
    }
    void seed_to_key_matches_known_vectors_across_all_parity_branches() {
        // Hand-verified by simulating cdbgengine.cpp's seed_to_key() in Python
        // (per-byte increment keyed on low 2 bits, 8-bit rotate-left-3, then a
        // parity-keyed byte permutation, then two 16-bit multiply-accumulates).
        // Each case below exercises a different parity (0-4) branch.
        QCOMPARE(seedToKey(0x00000000), quint32(0xAA04C31C)); // parity 0
        QCOMPARE(seedToKey(0x00000009), quint32(0x2104C31C)); // parity 1
        QCOMPARE(seedToKey(0x12345678), quint32(0x8C536B33)); // parity 2
        QCOMPARE(seedToKey(0x00090914), quint32(0x1E0C3241)); // parity 3
        QCOMPARE(seedToKey(0x09090909), quint32(0x1B632D75)); // parity 4 (default branch)
    }
    void extract_seed_reads_big_endian_bytes_4_to_7() {
        QCOMPARE(extractSeed(QByteArray::fromHex("0000000012345678")), quint32(0x12345678));
    }
    void extract_seed_returns_zero_for_short_reply() {
        QCOMPARE(extractSeed(QByteArray::fromHex("001122")), quint32(0));
    }
    void security_key_frame_layout() {
        QCOMPARE(buildSecurityKeyFrame(0x8C536B33), QByteArray::fromHex("13008C536B330000"));
    }
    void security_granted_checks_byte_3() {
        QVERIFY(securityGranted(QByteArray::fromHex("0000000100000000")));
        QVERIFY(!securityGranted(QByteArray::fromHex("0000000000000000")));
    }
    void security_granted_false_for_short_reply() {
        QVERIFY(!securityGranted(QByteArray::fromHex("0011")));
    }
    void log_reset_frame_layout() {
        QCOMPARE(buildLogResetFrame(0), QByteArray::fromHex("1400000000000631"));
        QCOMPARE(buildLogResetFrame(2), QByteArray::fromHex("1400020000000631"));
    }
    void log_start_frame_uses_milliseconds_when_it_fits_in_16_bits() {
        QCOMPARE(buildLogStartFrame(0, 1, 10), QByteArray::fromHex("060001000100000A"));
    }
    void log_start_frame_switches_to_tens_of_ms_above_65535() {
        // 100000 ms > 65535, so unit flag = 1 and the field carries 100000/10 = 10000 = 0x2710.
        QCOMPARE(buildLogStartFrame(0, 3, 100000), QByteArray::fromHex("0600010003012710"));
    }
};
int run_test_mitsu_colt_can_cdbg_protocol(int argc, char** argv) {
    TestMitsuColtCanCdbgProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_mitsu_colt_can_cdbg_protocol.moc"
```

- [ ] **Step 2: Wire the new test file into the test binary**

Edit `tests/tests.pro` — add to `SOURCES` (after `test_mitsu_colt_can_protocol.cpp`):

```
    test_mitsu_colt_can_cdbg_protocol.cpp \
    ../protocol/mitsu_colt_can_cdbg_protocol.cpp \
```

Add to `HEADERS` (after `test_mitsu_colt_can_protocol.h`):

```
    test_mitsu_colt_can_cdbg_protocol.h \
    ../protocol/mitsu_colt_can_cdbg_protocol.h \
```

Edit `tests/main.cpp` — add the include and call:

```cpp
#include "test_mitsu_colt_can_cdbg_protocol.h"
```

```cpp
    status |= run_test_mitsu_colt_can_cdbg_protocol(argc, argv);
```
(add this line right after the existing `run_test_mitsu_colt_can_protocol` call, before `return status;`)

- [ ] **Step 3: Run test to verify it fails**

Run: `cd tests && qmake6 tests.pro && make -j4`
Expected: FAIL to compile — `protocol/mitsu_colt_can_cdbg_protocol.h` does not exist yet.

- [ ] **Step 4: Write the header**

Create `protocol/mitsu_colt_can_cdbg_protocol.h`:

```cpp
#pragma once
#include <QByteArray>
#include <QVector>

// Mitsubishi Colt CZT (Z37A, ROM 47110032) Cdbg live-data logging protocol.
// Ported from externals/livemonitor/cdbgengine.cpp + logitemsmodel.cpp.
// Request CAN ID 0x630, reply CAN ID 0x631. Raw CAN (not ISO-TP): every
// command and reply is a single 8-byte frame.
namespace MitsuColtCanCdbg {

constexpr quint32 kRequestCanId = 0x630;
constexpr quint32 kReplyCanId   = 0x631;

constexpr quint8 kCmdInit          = 1;
constexpr quint8 kCmdSecuritySeed  = 18;
constexpr quint8 kCmdSecurityKey   = 19;
constexpr quint8 kCmdLogReset      = 20;
constexpr quint8 kCmdLogSelectItem = 21;
constexpr quint8 kCmdLogSetPointer = 22;
constexpr quint8 kCmdLogStart      = 6;

constexpr quint8 kSecurityLogAccess = 2;

// Every streamed reply frame reserves byte 0 as a frame-index marker, so a
// frame carries at most 7 payload bytes. getLogStartCommand's frameCount
// field matches cdbgengine.cpp's Q_ASSERT(frameCount <= 8): 8 frame slots.
constexpr int kMaxFrameBytes = 7;
constexpr int kMaxFrames     = 8;

// One logged RAM value: address + element size (1, 2, or 4 bytes).
struct CdbgChannel {
    quint32 pointer;
    quint8 size;
};

// {1, 1, 0,0,0,0,0,0}
QByteArray buildInitFrame();

// {18, 0, 2, 0,0,0,0,0} - kSecurityLogAccess seed request.
QByteArray buildSecuritySeedRequestFrame();

// Ported byte-for-byte from cdbgengine.cpp's seed_to_key(): per-byte
// increment (keyed on the byte's low 2 bits) + 8-bit left-rotate by 3,
// then a parity-keyed byte permutation, then two 16-bit multiply-accumulates.
// Pure integer math - no ROM-specific addresses involved.
quint32 seedToKey(quint32 seed);

// Extracts the big-endian 4-byte seed from the security-seed-request reply
// (reply bytes [4,8)). Returns 0 if reply is shorter than 8 bytes.
quint32 extractSeed(const QByteArray &reply);

// {19, 0, key[0..3] (big-endian), 0, 0}
QByteArray buildSecurityKeyFrame(quint32 key);

// True if the key-response reply grants access (byte[3] != 0) - matches
// livemonitor's security_flags check (the only documented success signal
// for this step, in mainwindow.cpp's dead-code cdbgSecurityAccessCallback).
// Returns false if reply is shorter than 4 bytes.
bool securityGranted(const QByteArray &reply);

// {20, 0, instance, 0, 0, 0, 0x06, 0x31}
QByteArray buildLogResetFrame(quint8 instance);

// {6, 0, 1, instance, frameCount, intervalUnit, intervalHi, intervalLo}.
// intervalMs is encoded directly in milliseconds if it fits in 16 bits
// (intervalUnit=0), otherwise in tens-of-milliseconds (intervalUnit=1,
// truncating division) - matches getLogStartCommand.
QByteArray buildLogStartFrame(quint8 instance, quint8 frameCount, quint32 intervalMs);

} // namespace MitsuColtCanCdbg
```

- [ ] **Step 5: Write the implementation**

Create `protocol/mitsu_colt_can_cdbg_protocol.cpp`:

```cpp
#include "protocol/mitsu_colt_can_cdbg_protocol.h"

namespace MitsuColtCanCdbg {

QByteArray buildInitFrame()
{
    QByteArray f;
    f.append(char(kCmdInit));
    f.append(char(1));
    for (int i = 0; i < 6; ++i) f.append(char(0));
    return f;
}

QByteArray buildSecuritySeedRequestFrame()
{
    QByteArray f;
    f.append(char(kCmdSecuritySeed));
    f.append(char(0));
    f.append(char(kSecurityLogAccess));
    for (int i = 0; i < 5; ++i) f.append(char(0));
    return f;
}

quint32 seedToKey(quint32 seed)
{
    quint8 data[4] = {
        quint8((seed >> 24) & 0xFF),
        quint8((seed >> 16) & 0xFF),
        quint8((seed >> 8) & 0xFF),
        quint8(seed & 0xFF)
    };

    for (int i = 0; i < 4; ++i) {
        quint8 x = data[i];
        switch (x & 0x03) {
        case 0: x = quint8(x + 145); break;
        case 1: x = quint8(x + 24);  break;
        case 2: x = quint8(x + 211); break;
        case 3: x = quint8(x + 2);   break;
        }
        data[i] = quint8((x << 3) | (x >> 5)); // 8-bit rotate-left by 3
    }

    int parity = (data[0] & 1) + (data[1] & 1) + (data[2] & 1) + (data[3] & 1);
    quint8 n[4];
    switch (parity) {
    case 0: n[0]=data[1]; n[1]=data[3]; n[2]=data[2]; n[3]=data[0]; break;
    case 1: n[0]=data[3]; n[1]=data[2]; n[2]=data[0]; n[3]=data[1]; break;
    case 2: n[0]=data[1]; n[1]=data[2]; n[2]=data[3]; n[3]=data[0]; break;
    case 3: n[0]=data[1]; n[1]=data[0]; n[2]=data[2]; n[3]=data[3]; break;
    default: n[0]=data[2]; n[1]=data[0]; n[2]=data[1]; n[3]=data[3]; break;
    }

    quint16 word0 = quint16(((n[0] << 8) + n[1]) * 3 + n[3] * 8);
    quint16 word1 = quint16(((n[2] << 8) + n[3]) * 5 + n[1] * 8);

    return (quint32(word0 >> 8) << 24) | (quint32(word0 & 0xFF) << 16)
         | (quint32(word1 >> 8) << 8)  |  quint32(word1 & 0xFF);
}

quint32 extractSeed(const QByteArray &reply)
{
    if (reply.size() < 8) return 0;
    return (quint32(quint8(reply.at(4))) << 24) | (quint32(quint8(reply.at(5))) << 16)
         | (quint32(quint8(reply.at(6))) << 8)  |  quint32(quint8(reply.at(7)));
}

QByteArray buildSecurityKeyFrame(quint32 key)
{
    QByteArray f;
    f.append(char(kCmdSecurityKey));
    f.append(char(0));
    f.append(char((key >> 24) & 0xFF));
    f.append(char((key >> 16) & 0xFF));
    f.append(char((key >> 8) & 0xFF));
    f.append(char(key & 0xFF));
    f.append(char(0));
    f.append(char(0));
    return f;
}

bool securityGranted(const QByteArray &reply)
{
    if (reply.size() < 4) return false;
    return quint8(reply.at(3)) != 0;
}

QByteArray buildLogResetFrame(quint8 instance)
{
    QByteArray f;
    f.append(char(kCmdLogReset));
    f.append(char(0));
    f.append(char(instance));
    f.append(char(0));
    f.append(char(0));
    f.append(char(0));
    f.append(char(0x06));
    f.append(char(0x31));
    return f;
}

QByteArray buildLogStartFrame(quint8 instance, quint8 frameCount, quint32 intervalMs)
{
    quint8 unitFlag;
    quint16 encoded;
    if (intervalMs > 65535) {
        unitFlag = 1;
        encoded = quint16(intervalMs / 10);
    } else {
        unitFlag = 0;
        encoded = quint16(intervalMs);
    }

    QByteArray f;
    f.append(char(kCmdLogStart));
    f.append(char(0));
    f.append(char(1));
    f.append(char(instance));
    f.append(char(frameCount));
    f.append(char(unitFlag));
    f.append(char((encoded >> 8) & 0xFF));
    f.append(char(encoded & 0xFF));
    return f;
}

} // namespace MitsuColtCanCdbg
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cd tests && qmake6 tests.pro && make -j4 && ./mut_dma_tests`
Expected: `TestMitsuColtCanCdbgProtocol` section shows all cases PASS, `Totals: 13 passed, 0 failed` (11 named slots plus QtTest's automatic `initTestCase`/`cleanupTestCase`), and no other suite regresses.

- [ ] **Step 7: Commit**

```bash
git add protocol/mitsu_colt_can_cdbg_protocol.h protocol/mitsu_colt_can_cdbg_protocol.cpp \
        tests/test_mitsu_colt_can_cdbg_protocol.h tests/test_mitsu_colt_can_cdbg_protocol.cpp \
        tests/tests.pro tests/main.cpp
git commit -m "feat: add Cdbg session/security protocol codec for Colt CZT CAN logging"
```

---

### Task 2: Cdbg protocol codec — frame batching, configuration & decode

**Files:**
- Modify: `protocol/mitsu_colt_can_cdbg_protocol.h`
- Modify: `protocol/mitsu_colt_can_cdbg_protocol.cpp`
- Modify: `tests/test_mitsu_colt_can_cdbg_protocol.cpp`

**Interfaces:**
- Consumes: `MitsuColtCanCdbg::CdbgChannel`, `kMaxFrameBytes`, `kMaxFrames`, `kCmdLogSelectItem`, `kCmdLogSetPointer` (Task 1).
- Produces: `bool batchChannelsIntoFrames(const QVector<CdbgChannel> &channels, QVector<QVector<CdbgChannel>> &outFrames)`, `QVector<QByteArray> buildFrameInitFrames(quint8 instance, quint8 frameIndex, const QVector<CdbgChannel> &frameItems)`, `QVector<quint32> decodeFrame(quint8 expectedFrameIndex, const QVector<CdbgChannel> &frameItems, const QByteArray &frame)`.

- [ ] **Step 1: Write the failing test**

Edit `tests/test_mitsu_colt_can_cdbg_protocol.cpp` — add these slots inside `TestMitsuColtCanCdbgProtocol` (before the closing `};`):

```cpp
    void batching_packs_channels_into_one_frame_when_they_fit() {
        QVector<CdbgChannel> channels = { {0x804FBF, 1}, {0x804DF2, 2} };
        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(batchChannelsIntoFrames(channels, frames));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).size(), 2);
    }
    void batching_starts_a_new_frame_when_the_next_channel_would_overflow() {
        // byteIndex starts at 1 (byte 0 is the frame-index marker). Two
        // 4-byte channels can't share a frame (1+4=5 ok, but the second
        // 4-byte channel would need 5+4=9 > 8), so it spills into frame 1
        // along with the following 2-byte channel (5+2=7 <= 8).
        QVector<CdbgChannel> channels = { {0x804FBF, 4}, {0x804DF2, 4}, {0x8054AC, 2} };
        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(batchChannelsIntoFrames(channels, frames));
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames.at(0).size(), 1);
        QCOMPARE(frames.at(1).size(), 2);
    }
    void batching_rejects_empty_channel_list() {
        QVector<CdbgChannel> channels;
        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(!batchChannelsIntoFrames(channels, frames));
    }
    void batching_rejects_more_than_kMaxFrames_frames() {
        // 9 single-byte-incompatible channels forcing 9 frames (one 4-byte
        // channel per frame, since 1+4=5 <= 8 but a second 4-byte channel
        // would overflow) - one more than kMaxFrames (8).
        QVector<CdbgChannel> channels;
        for (int i = 0; i < 9; ++i)
            channels.append(CdbgChannel{quint32(0x800000 + i), 4});
        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(!batchChannelsIntoFrames(channels, frames));
    }
    void frame_init_frames_layout_for_two_items() {
        QVector<CdbgChannel> frameItems = { {0x804FBF, 1}, {0x804DF2, 2} };
        QVector<QByteArray> cmds = buildFrameInitFrames(0, 0, frameItems);
        QCOMPARE(cmds.size(), 4);
        QCOMPARE(cmds.at(0), QByteArray::fromHex("1500000000000000")); // select item 0
        QCOMPARE(cmds.at(1), QByteArray::fromHex("1600010000804FBF")); // pointer/size item 0
        QCOMPARE(cmds.at(2), QByteArray::fromHex("1500000001000000")); // select item 1
        QCOMPARE(cmds.at(3), QByteArray::fromHex("1600020000804DF2")); // pointer/size item 1
    }
    void decode_frame_reads_big_endian_values_at_the_right_offsets() {
        QVector<CdbgChannel> frameItems = { {0x804FBF, 1}, {0x804DF2, 2} };
        QByteArray frame = QByteArray::fromHex("002A123400000000");
        QVector<quint32> values = decodeFrame(0, frameItems, frame);
        QCOMPARE(values.size(), 2);
        QCOMPARE(values.at(0), quint32(0x2A));
        QCOMPARE(values.at(1), quint32(0x1234));
    }
    void decode_frame_rejects_mismatched_frame_index() {
        QVector<CdbgChannel> frameItems = { {0x804FBF, 1} };
        QByteArray frame = QByteArray::fromHex("012A000000000000"); // index byte is 1, not 0
        QVERIFY(decodeFrame(0, frameItems, frame).isEmpty());
    }
    void decode_frame_rejects_too_short_frame() {
        QVector<CdbgChannel> frameItems = { {0x804FBF, 4} };
        QByteArray frame = QByteArray::fromHex("0011"); // needs 1+4=5 bytes, only has 2
        QVERIFY(decodeFrame(0, frameItems, frame).isEmpty());
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tests && qmake6 tests.pro && make -j4`
Expected: FAIL to compile — `batchChannelsIntoFrames`, `buildFrameInitFrames`, `decodeFrame` are not declared.

- [ ] **Step 3: Add the declarations**

Edit `protocol/mitsu_colt_can_cdbg_protocol.h` — add before the closing `} // namespace MitsuColtCanCdbg`:

```cpp
// Packs channels into frames of at most kMaxFrameBytes total size each (byte
// 0 of every streamed frame is the frame-index marker, so payload starts at
// byte 1), at most kMaxFrames frames, matching
// LogItemsModel::loadSchema's byteIndex/frameIndex bookkeeping. Returns
// false (outFrames untouched) if channels is empty or does not fit within
// kMaxFrames frames.
bool batchChannelsIntoFrames(const QVector<CdbgChannel> &channels,
                              QVector<QVector<CdbgChannel>> &outFrames);

// Builds the {21,...}/{22,...} command pairs (in order: select0, pointer0,
// select1, pointer1, ...) that configure one frame's items, matching
// getLogFrameInitCommands. Caller must have already produced frameItems via
// batchChannelsIntoFrames.
QVector<QByteArray> buildFrameInitFrames(quint8 instance, quint8 frameIndex,
                                          const QVector<CdbgChannel> &frameItems);

// Decodes one streamed reply frame (byte 0 = frame index, bytes [1, N) =
// concatenated big-endian channel values per frameItems, in order) into one
// raw unsigned value per channel. Returns an empty vector if frame is
// shorter than 1 byte, frame[0] doesn't match expectedFrameIndex, or frame
// is too short to hold every channel in frameItems.
QVector<quint32> decodeFrame(quint8 expectedFrameIndex,
                              const QVector<CdbgChannel> &frameItems,
                              const QByteArray &frame);
```

- [ ] **Step 4: Implement the functions**

Edit `protocol/mitsu_colt_can_cdbg_protocol.cpp` — add before the closing `} // namespace MitsuColtCanCdbg`:

```cpp
bool batchChannelsIntoFrames(const QVector<CdbgChannel> &channels,
                              QVector<QVector<CdbgChannel>> &outFrames)
{
    if (channels.isEmpty()) return false;

    QVector<QVector<CdbgChannel>> frames;
    QVector<CdbgChannel> current;
    int byteIndex = 1;

    for (const CdbgChannel &ch : channels) {
        if (byteIndex + ch.size > 8) {
            frames.append(current);
            current.clear();
            byteIndex = 1;
        }
        current.append(ch);
        byteIndex += ch.size;
    }
    if (!current.isEmpty())
        frames.append(current);

    if (frames.size() > kMaxFrames)
        return false;

    outFrames = frames;
    return true;
}

QVector<QByteArray> buildFrameInitFrames(quint8 instance, quint8 frameIndex,
                                          const QVector<CdbgChannel> &frameItems)
{
    QVector<QByteArray> out;
    for (int i = 0; i < frameItems.size(); ++i) {
        QByteArray select;
        select.append(char(kCmdLogSelectItem));
        select.append(char(0));
        select.append(char(instance));
        select.append(char(frameIndex));
        select.append(char(i));
        select.append(char(0));
        select.append(char(0));
        select.append(char(0));
        out.append(select);

        const CdbgChannel &ch = frameItems.at(i);
        QByteArray pointer;
        pointer.append(char(kCmdLogSetPointer));
        pointer.append(char(0));
        pointer.append(char(ch.size));
        pointer.append(char(0));
        pointer.append(char((ch.pointer >> 24) & 0xFF));
        pointer.append(char((ch.pointer >> 16) & 0xFF));
        pointer.append(char((ch.pointer >> 8) & 0xFF));
        pointer.append(char(ch.pointer & 0xFF));
        out.append(pointer);
    }
    return out;
}

QVector<quint32> decodeFrame(quint8 expectedFrameIndex,
                              const QVector<CdbgChannel> &frameItems,
                              const QByteArray &frame)
{
    if (frame.size() < 1 || quint8(frame.at(0)) != expectedFrameIndex)
        return {};

    int need = 1;
    for (const CdbgChannel &ch : frameItems)
        need += ch.size;
    if (frame.size() < need)
        return {};

    QVector<quint32> out;
    int offset = 1;
    for (const CdbgChannel &ch : frameItems) {
        quint32 value = 0;
        for (int k = 0; k < ch.size; ++k)
            value = (value << 8) | quint32(quint8(frame.at(offset + k)));
        out.append(value);
        offset += ch.size;
    }
    return out;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd tests && qmake6 tests.pro && make -j4 && ./mut_dma_tests`
Expected: `TestMitsuColtCanCdbgProtocol` shows `Totals: 21 passed, 0 failed` (19 named slots plus `initTestCase`/`cleanupTestCase`), no other suite regresses.

- [ ] **Step 6: Commit**

```bash
git add protocol/mitsu_colt_can_cdbg_protocol.h protocol/mitsu_colt_can_cdbg_protocol.cpp \
        tests/test_mitsu_colt_can_cdbg_protocol.cpp
git commit -m "feat: add Cdbg frame batching, configuration and decode to protocol codec"
```

---

### Task 3: Raw CAN transport abstraction

**Files:**
- Create: `protocol/ican_transport.h`
- Create: `protocol/fastecu_can_transport.h`
- Create: `protocol/fastecu_can_transport.cpp`
- Create: `tests/scripted_can_transport.h`
- Modify: `tests/tests.pro`
- Modify: `FastECU.pro`

**Interfaces:**
- Produces: `cdbg::ICanTransport` (`write(quint32 canId, const QByteArray&)`, `read(int timeoutMs, quint32 &outId)`), `cdbg::FastEcuCanTransport` (concrete adapter over `SerialPortActions*`), `cdbg::ScriptedCanTransport` (test double, header-only, used by Task 4).

There is no dedicated unit test for `FastEcuCanTransport` itself — it wraps live hardware I/O, and this codebase's existing precedent (`FastEcuKlineTransport`, the MUT/DMA equivalent) has none either; only the interface and test double are exercised, via the driver tests in Task 4. This task's own verification is a successful build.

- [ ] **Step 1: Write the interface**

Create `protocol/ican_transport.h`:

```cpp
#pragma once
#include <QByteArray>

namespace cdbg {

// Raw-CAN transport (not ISO-TP): every write/read is one CAN frame,
// addressed by an explicit 11-bit arbitration ID.
class ICanTransport {
public:
    virtual ~ICanTransport() = default;
    // Sends `payload` (up to 8 bytes) on CAN id `canId`. Returns bytes written.
    virtual int write(quint32 canId, const QByteArray &payload) = 0;
    // Reads one CAN frame within timeoutMs. Returns the frame's payload and
    // sets outId to its CAN id, or returns an empty QByteArray if nothing
    // arrived in time.
    virtual QByteArray read(int timeoutMs, quint32 &outId) = 0;
};

} // namespace cdbg
```

- [ ] **Step 2: Write the test double**

Create `tests/scripted_can_transport.h`:

```cpp
#pragma once
#include "protocol/ican_transport.h"
#include <QList>
namespace cdbg {
// Test double: assert the exact sequence of (id,payload) writes, feed canned
// (id,payload) reads in order.
class ScriptedCanTransport : public ICanTransport {
public:
    void expectWrite(quint32 id, const QByteArray &payload) { expectedIds_.append(id); expectedPayloads_.append(payload); }
    void queueRead(quint32 id, const QByteArray &payload) { readIds_.append(id); readPayloads_.append(payload); }
    bool scriptConsumed() const { return wIdx_ == expectedIds_.size() && rIdx_ == readIds_.size(); }
    bool ok() const { return ok_; }
    int write(quint32 id, const QByteArray &payload) override {
        if (wIdx_ >= expectedIds_.size() || expectedIds_.at(wIdx_) != id || expectedPayloads_.at(wIdx_) != payload)
            ok_ = false;
        else
            ++wIdx_;
        return payload.size();
    }
    QByteArray read(int, quint32 &outId) override {
        if (rIdx_ >= readIds_.size())
            return QByteArray();
        outId = readIds_.at(rIdx_);
        return readPayloads_.at(rIdx_++);
    }
private:
    QList<quint32> expectedIds_, readIds_;
    QList<QByteArray> expectedPayloads_, readPayloads_;
    int wIdx_ = 0, rIdx_ = 0;
    bool ok_ = true;
};
}
```

- [ ] **Step 3: Write the concrete FastECU adapter**

Create `protocol/fastecu_can_transport.h`:

```cpp
#pragma once
#include "protocol/ican_transport.h"
class SerialPortActions;
namespace cdbg {
// Adapts FastECU's SerialPortActions raw-CAN mode to ICanTransport. Matches
// the wire convention already used by
// modules/ecu/flash_ecu_subaru_denso_sh705x_densocan.cpp: each frame is 4
// big-endian CAN-id bytes followed by the payload.
class FastEcuCanTransport : public ICanTransport {
public:
    explicit FastEcuCanTransport(SerialPortActions *serial) : serial_(serial) {}
    int write(quint32 canId, const QByteArray &payload) override;
    QByteArray read(int timeoutMs, quint32 &outId) override;
private:
    SerialPortActions *serial_;
};
}
```

Create `protocol/fastecu_can_transport.cpp`:

```cpp
#include "protocol/fastecu_can_transport.h"
#include "serial_port/serial_port_actions.h"

namespace cdbg {

int FastEcuCanTransport::write(quint32 canId, const QByteArray &payload)
{
    QByteArray frame;
    frame.append(char((canId >> 24) & 0xFF));
    frame.append(char((canId >> 16) & 0xFF));
    frame.append(char((canId >> 8) & 0xFF));
    frame.append(char(canId & 0xFF));
    frame.append(payload);
    serial_->write_serial_data_echo_check(frame);
    return payload.size();
}

QByteArray FastEcuCanTransport::read(int timeoutMs, quint32 &outId)
{
    QByteArray raw = serial_->read_serial_data(quint16(timeoutMs));
    if (raw.size() < 4)
        return QByteArray();
    outId = (quint32(quint8(raw.at(0))) << 24) | (quint32(quint8(raw.at(1))) << 16)
          | (quint32(quint8(raw.at(2))) << 8)  |  quint32(quint8(raw.at(3)));
    return raw.mid(4);
}

}
```

- [ ] **Step 4: Wire into the test binary's header list (for Task 4 to use) and into FastECU.pro**

Edit `tests/tests.pro` — add to `HEADERS` (after the cdbg protocol header added in Task 1):

```
    scripted_can_transport.h \
    ../protocol/ican_transport.h \
```

Edit `FastECU.pro` — add to `SOURCES` (after `protocol/fastecu_kline_transport.cpp`, matching that line's trailing-backslash style — see Step 5 for the exact edit):

```
    protocol/mitsu_colt_can_cdbg_protocol.cpp \
    protocol/fastecu_can_transport.cpp
```

Add to `HEADERS` (after `protocol/fastecu_kline_transport.h`):

```
    protocol/mitsu_colt_can_cdbg_protocol.h \
    protocol/ican_transport.h \
    protocol/fastecu_can_transport.h
```

- [ ] **Step 5: Apply the FastECU.pro edit precisely**

`FastECU.pro`'s `SOURCES` list currently ends with:

```
    protocol/imut_dma_init.cpp \
    protocol/mut_dma_driver.cpp \
    protocol/fastecu_kline_transport.cpp
```

Change the last line so the list continues, becoming:

```
    protocol/imut_dma_init.cpp \
    protocol/mut_dma_driver.cpp \
    protocol/fastecu_kline_transport.cpp \
    protocol/mitsu_colt_can_cdbg_protocol.cpp \
    protocol/fastecu_can_transport.cpp
```

`FastECU.pro`'s `HEADERS` list currently ends with:

```
    protocol/imut_dma_init.h \
    protocol/mut_dma_driver.h \
    protocol/fastecu_kline_transport.h
```

Change it to:

```
    protocol/imut_dma_init.h \
    protocol/mut_dma_driver.h \
    protocol/fastecu_kline_transport.h \
    protocol/mitsu_colt_can_cdbg_protocol.h \
    protocol/ican_transport.h \
    protocol/fastecu_can_transport.h
```

- [ ] **Step 6: Run the test suite build to verify nothing broke**

Run: `cd tests && qmake6 tests.pro && make -j4 && ./mut_dma_tests`
Expected: builds clean, all suites still pass (the new header-only `ICanTransport`/`ScriptedCanTransport` aren't exercised by any test yet — that's Task 4 — but they must compile when included).

Run: `qmake6 FastECU.pro`
Expected: regenerates the Makefile without error (full `make` of the GUI app is not required for this task; Task 5's integration step is where the new adapter is actually used and where a full build is verified).

- [ ] **Step 7: Commit**

```bash
git add protocol/ican_transport.h protocol/fastecu_can_transport.h protocol/fastecu_can_transport.cpp \
        tests/scripted_can_transport.h tests/tests.pro FastECU.pro
git commit -m "feat: add raw CAN transport abstraction for Cdbg logging"
```

---

### Task 4: CdbgLogDriver — session orchestration and streaming poll

**Files:**
- Create: `protocol/mitsu_colt_can_cdbg_driver.h`
- Create: `protocol/mitsu_colt_can_cdbg_driver.cpp`
- Create: `tests/test_cdbg_driver.h`
- Create: `tests/test_cdbg_driver.cpp`
- Modify: `tests/tests.pro`
- Modify: `tests/main.cpp`
- Modify: `FastECU.pro`

**Interfaces:**
- Consumes: `cdbg::ICanTransport`, `cdbg::ScriptedCanTransport` (Task 3); `MitsuColtCanCdbg::{CdbgChannel, kRequestCanId, kReplyCanId, buildInitFrame, buildSecuritySeedRequestFrame, seedToKey, extractSeed, buildSecurityKeyFrame, securityGranted, buildLogResetFrame, batchChannelsIntoFrames, buildFrameInitFrames, buildLogStartFrame, decodeFrame}` (Tasks 1-2).
- Produces: `MitsuColtCanCdbg::CdbgLogDriver` — `explicit CdbgLogDriver(cdbg::ICanTransport &transport)`, `bool startFreeFormLog(const QVector<CdbgChannel> &channels, quint8 instance = 0, quint32 intervalMs = 10)`, `bool isStreaming() const`, `QVector<quint32> pollOnce(int timeoutMs)`. Consumed by Task 5's `log_operations_mitsubishi_cdbg.cpp`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_cdbg_driver.h`:

```cpp
#pragma once
int run_test_cdbg_driver(int argc, char** argv);
```

Create `tests/test_cdbg_driver.cpp`:

```cpp
#include <QtTest>
#include "protocol/mitsu_colt_can_cdbg_driver.h"
#include "scripted_can_transport.h"
#include "test_cdbg_driver.h"
using namespace MitsuColtCanCdbg;

class TestCdbgDriver : public QObject { Q_OBJECT
private slots:
    void handshake_and_single_frame_streaming() {
        cdbg::ScriptedCanTransport t;
        QVector<CdbgChannel> ch = { {0x804FBF, 1}, {0x804DF2, 2} };

        t.expectWrite(kRequestCanId, buildInitFrame());
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000000000000"));

        t.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000012345678")); // seed=0x12345678

        t.expectWrite(kRequestCanId, buildSecurityKeyFrame(0x8C536B33)); // seedToKey(0x12345678)
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000100000000")); // byte3 != 0 -> granted

        t.expectWrite(kRequestCanId, buildLogResetFrame(0));
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000000000000"));

        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(batchChannelsIntoFrames(ch, frames));
        QCOMPARE(frames.size(), 1);
        for (const QByteArray &cmd : buildFrameInitFrames(0, 0, frames.at(0))) {
            t.expectWrite(kRequestCanId, cmd);
            t.queueRead(kReplyCanId, QByteArray::fromHex("0000000000000000"));
        }

        t.expectWrite(kRequestCanId, buildLogStartFrame(0, 1, 10));
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000000000000"));

        CdbgLogDriver d(t);
        QVERIFY(d.startFreeFormLog(ch, 0, 10));
        QVERIFY(d.isStreaming());
        QVERIFY(t.scriptConsumed());
        QVERIFY(t.ok());

        t.queueRead(kReplyCanId, QByteArray::fromHex("002A123400000000"));
        QVector<quint32> vals = d.pollOnce(50);
        QCOMPARE(vals.size(), 2);
        QCOMPARE(vals.at(0), quint32(42));
        QCOMPARE(vals.at(1), quint32(0x1234));
    }

    void handshake_fails_when_security_not_granted() {
        cdbg::ScriptedCanTransport t;
        QVector<CdbgChannel> ch = { {0x804FBF, 1} };

        t.expectWrite(kRequestCanId, buildInitFrame());
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000000000000"));
        t.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000000000000")); // seed=0
        t.expectWrite(kRequestCanId, buildSecurityKeyFrame(seedToKey(0)));
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000000000000")); // byte3 == 0 -> denied

        CdbgLogDriver d(t);
        QVERIFY(!d.startFreeFormLog(ch, 0, 10));
        QVERIFY(!d.isStreaming());
    }

    void handshake_fails_when_init_gets_no_reply() {
        cdbg::ScriptedCanTransport t;
        QVector<CdbgChannel> ch = { {0x804FBF, 1} };
        t.expectWrite(kRequestCanId, buildInitFrame());
        // no queued read -> transport returns empty payload -> handshake must fail here.
        CdbgLogDriver d(t);
        QVERIFY(!d.startFreeFormLog(ch, 0, 10));
        QVERIFY(!d.isStreaming());
    }

    void poll_merges_values_across_two_frames() {
        cdbg::ScriptedCanTransport t;
        QVector<CdbgChannel> ch = { {0x804FBF, 4}, {0x804DF2, 4}, {0x8054AC, 2} };

        t.expectWrite(kRequestCanId, buildInitFrame());
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000000000000"));
        t.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000012345678"));
        t.expectWrite(kRequestCanId, buildSecurityKeyFrame(0x8C536B33));
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000100000000"));
        t.expectWrite(kRequestCanId, buildLogResetFrame(0));
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000000000000"));

        QVector<QVector<CdbgChannel>> frames;
        QVERIFY(batchChannelsIntoFrames(ch, frames));
        QCOMPARE(frames.size(), 2);
        for (int f = 0; f < frames.size(); ++f) {
            for (const QByteArray &cmd : buildFrameInitFrames(0, quint8(f), frames.at(f))) {
                t.expectWrite(kRequestCanId, cmd);
                t.queueRead(kReplyCanId, QByteArray::fromHex("0000000000000000"));
            }
        }
        t.expectWrite(kRequestCanId, buildLogStartFrame(0, 2, 10));
        t.queueRead(kReplyCanId, QByteArray::fromHex("0000000000000000"));

        CdbgLogDriver d(t);
        QVERIFY(d.startFreeFormLog(ch, 0, 10));

        // Frame 0 arrives first: channel 0 (4-byte) = 0xAABBCCDD.
        t.queueRead(kReplyCanId, QByteArray::fromHex("00AABBCCDD000000"));
        QVector<quint32> v1 = d.pollOnce(50);
        QCOMPARE(v1.size(), 3);
        QCOMPARE(v1.at(0), quint32(0xAABBCCDD));
        QCOMPARE(v1.at(1), quint32(0));
        QCOMPARE(v1.at(2), quint32(0));

        // Frame 1 arrives next: channel 1 (4-byte) = 0x11223344, channel 2 (2-byte) = 0x5566.
        t.queueRead(kReplyCanId, QByteArray::fromHex("0111223344556600"));
        QVector<quint32> v2 = d.pollOnce(50);
        QCOMPARE(v2.size(), 3);
        QCOMPARE(v2.at(0), quint32(0xAABBCCDD)); // retained from frame 0
        QCOMPARE(v2.at(1), quint32(0x11223344));
        QCOMPARE(v2.at(2), quint32(0x5566));
    }

    void poll_returns_empty_when_not_streaming() {
        cdbg::ScriptedCanTransport t;
        CdbgLogDriver d(t);
        QVERIFY(d.pollOnce(50).isEmpty());
    }
};
int run_test_cdbg_driver(int argc, char** argv) {
    TestCdbgDriver t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_cdbg_driver.moc"
```

- [ ] **Step 2: Wire into the test binary**

Edit `tests/tests.pro` — add to `SOURCES` (after the cdbg protocol codec entry from Task 1):

```
    test_cdbg_driver.cpp \
    ../protocol/mitsu_colt_can_cdbg_driver.cpp \
```

Add to `HEADERS` (after the cdbg protocol codec header entry):

```
    test_cdbg_driver.h \
    ../protocol/mitsu_colt_can_cdbg_driver.h \
```

Edit `tests/main.cpp` — add the include and call:

```cpp
#include "test_cdbg_driver.h"
```

```cpp
    status |= run_test_cdbg_driver(argc, argv);
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd tests && qmake6 tests.pro && make -j4`
Expected: FAIL to compile — `protocol/mitsu_colt_can_cdbg_driver.h` does not exist yet.

- [ ] **Step 4: Write the header**

Create `protocol/mitsu_colt_can_cdbg_driver.h`:

```cpp
#pragma once
#include "protocol/ican_transport.h"
#include "protocol/mitsu_colt_can_cdbg_protocol.h"
#include <QVector>

namespace MitsuColtCanCdbg {

class CdbgLogDriver {
public:
    explicit CdbgLogDriver(cdbg::ICanTransport &transport) : t_(transport) {}

    // Runs the session-init + seed/key security handshake + frame
    // configuration + start command for `channels` (RAM pointer + size
    // each). Returns true once the ECU has been told to start streaming.
    bool startFreeFormLog(const QVector<CdbgChannel> &channels,
                           quint8 instance = 0, quint32 intervalMs = 10);
    bool isStreaming() const { return streaming_; }

    // Reads at most one streamed frame (within timeoutMs) and merges any
    // decoded channel values into this driver's per-channel cache. Returns
    // the full per-channel raw value vector (using cached/last-known values
    // for channels whose frame hasn't arrived since streaming started, 0
    // until then), or an empty vector if not currently streaming.
    QVector<quint32> pollOnce(int timeoutMs);

private:
    cdbg::ICanTransport &t_;
    QVector<QVector<CdbgChannel>> frames_;
    QVector<quint32> lastValues_;
    bool streaming_ = false;
};

} // namespace MitsuColtCanCdbg
```

- [ ] **Step 5: Write the implementation**

Create `protocol/mitsu_colt_can_cdbg_driver.cpp`:

```cpp
#include "protocol/mitsu_colt_can_cdbg_driver.h"

namespace MitsuColtCanCdbg {

namespace {
bool sendAndReceive(cdbg::ICanTransport &t, const QByteArray &cmd, QByteArray &outReply)
{
    t.write(kRequestCanId, cmd);
    quint32 id = 0;
    outReply = t.read(250, id);
    return !outReply.isEmpty() && id == kReplyCanId;
}
}

bool CdbgLogDriver::startFreeFormLog(const QVector<CdbgChannel> &channels,
                                      quint8 instance, quint32 intervalMs)
{
    streaming_ = false;
    frames_.clear();
    lastValues_.clear();

    QByteArray reply;
    if (!sendAndReceive(t_, buildInitFrame(), reply))
        return false;

    if (!sendAndReceive(t_, buildSecuritySeedRequestFrame(), reply))
        return false;
    quint32 key = seedToKey(extractSeed(reply));
    if (!sendAndReceive(t_, buildSecurityKeyFrame(key), reply))
        return false;
    if (!securityGranted(reply))
        return false;

    if (!batchChannelsIntoFrames(channels, frames_))
        return false;

    if (!sendAndReceive(t_, buildLogResetFrame(instance), reply))
        return false;

    for (int f = 0; f < frames_.size(); ++f) {
        const QVector<QByteArray> cmds = buildFrameInitFrames(instance, quint8(f), frames_.at(f));
        for (const QByteArray &cmd : cmds) {
            if (!sendAndReceive(t_, cmd, reply))
                return false;
        }
    }

    if (!sendAndReceive(t_, buildLogStartFrame(instance, quint8(frames_.size()), intervalMs), reply))
        return false;

    int totalChannels = 0;
    for (const auto &frame : frames_)
        totalChannels += frame.size();
    lastValues_.fill(0, totalChannels);

    streaming_ = true;
    return true;
}

QVector<quint32> CdbgLogDriver::pollOnce(int timeoutMs)
{
    if (!streaming_)
        return {};

    quint32 id = 0;
    QByteArray frame = t_.read(timeoutMs, id);
    if (!frame.isEmpty() && id == kReplyCanId) {
        quint8 frameIdx = quint8(frame.at(0));
        if (frameIdx < quint8(frames_.size())) {
            QVector<quint32> decoded = decodeFrame(frameIdx, frames_.at(frameIdx), frame);
            if (!decoded.isEmpty()) {
                int offset = 0;
                for (int f = 0; f < frameIdx; ++f)
                    offset += frames_.at(f).size();
                for (int i = 0; i < decoded.size(); ++i)
                    lastValues_[offset + i] = decoded.at(i);
            }
        }
    }
    return lastValues_;
}

} // namespace MitsuColtCanCdbg
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cd tests && qmake6 tests.pro && make -j4 && ./mut_dma_tests`
Expected: `TestCdbgDriver` shows `Totals: 7 passed, 0 failed` (5 named slots plus `initTestCase`/`cleanupTestCase`), no other suite regresses.

- [ ] **Step 7: Wire the driver into FastECU.pro**

Edit `FastECU.pro` — extend the `SOURCES` edit from Task 3 so the tail reads:

```
    protocol/fastecu_kline_transport.cpp \
    protocol/mitsu_colt_can_cdbg_protocol.cpp \
    protocol/fastecu_can_transport.cpp \
    protocol/mitsu_colt_can_cdbg_driver.cpp
```

Extend the `HEADERS` edit from Task 3 so the tail reads:

```
    protocol/fastecu_kline_transport.h \
    protocol/mitsu_colt_can_cdbg_protocol.h \
    protocol/ican_transport.h \
    protocol/fastecu_can_transport.h \
    protocol/mitsu_colt_can_cdbg_driver.h
```

Run: `qmake6 FastECU.pro`
Expected: regenerates the Makefile without error.

- [ ] **Step 8: Commit**

```bash
git add protocol/mitsu_colt_can_cdbg_driver.h protocol/mitsu_colt_can_cdbg_driver.cpp \
        tests/test_cdbg_driver.h tests/test_cdbg_driver.cpp tests/tests.pro tests/main.cpp FastECU.pro
git commit -m "feat: add CdbgLogDriver session orchestration and streaming poll"
```

---

### Task 5: FastECU integration — wire Cdbg into the Logging screen

**Files:**
- Create: `log_operations_mitsubishi_cdbg.cpp`
- Modify: `mainwindow.h`
- Modify: `menu_actions.cpp`
- Modify: `mainwindow.cpp`
- Modify: `FastECU.pro`

**Interfaces:**
- Consumes: `MitsuColtCanCdbg::CdbgLogDriver`, `MitsuColtCanCdbg::CdbgChannel` (Task 4); `cdbg::FastEcuCanTransport` (Task 3); `FileActions::LogValuesStructure` (`log_value_protocol`, `log_value_id`, `lower_panel_log_value_id`, `log_value_address`, `log_value_length`, `log_value_enabled`, `log_value_units`, `log_value`, all `QStringList`, existing in `file_actions.h`); `MainWindow::logparams_poll_timer`, `MainWindow::serial`, `MainWindow::fileActions`, `MainWindow::logValues` (all existing `MainWindow` members).
- Produces: `MainWindow::cdbg_start_logging()`, `MainWindow::cdbg_stop_logging()`, `MainWindow::cdbg_poll()` — same call shape as the existing `mitsubishi_dma_start_logging()` / `mitsubishi_dma_stop_logging()` / `mitsubishi_dma_poll()` trio, dispatched from the same places.

There is no automated test for this task — it wires driver logic into live `MainWindow`/`SerialPortActions` state, same situation as the existing MUT/DMA integration (`log_operations_mitsubishi.cpp` has no dedicated test file either). Verification is a full application build.

- [ ] **Step 1: Add member declarations to mainwindow.h**

Edit `mainwindow.h` — after the existing MUT/DMA includes (around line 100):

```cpp
#include "protocol/mut_dma_driver.h"
#include "protocol/fastecu_kline_transport.h"
#include "protocol/imut_dma_init.h"
```

add:

```cpp
#include "protocol/mitsu_colt_can_cdbg_driver.h"
#include "protocol/fastecu_can_transport.h"
```

Edit `mainwindow.h` — after the existing MUT/DMA member declarations (around line 258):

```cpp
    mutdma::MutDmaDriver* mut_driver = nullptr;
    mutdma::FastEcuKlineTransport* mut_transport = nullptr;
    mutdma::IMutDmaInit* mut_init = nullptr;
```

add:

```cpp
    MitsuColtCanCdbg::CdbgLogDriver* cdbg_driver = nullptr;
    cdbg::FastEcuCanTransport* cdbg_transport = nullptr;
```

Edit `mainwindow.h` — after the existing MUT/DMA method declarations under `// log_operations_mitsubishi (MUT/DMA)` (around line 320-324):

```cpp
    // log_operations_mitsubishi (MUT/DMA)
    void mitsubishi_dma_start_logging();
    void mitsubishi_dma_stop_logging();
    bool mut_write_memory(quint16 addr, const QByteArray& bytes);
    QByteArray mut_read_memory(quint16 addr, int len);
```

add:

```cpp

    // log_operations_mitsubishi_cdbg (Cdbg CAN logging, Colt CZT 47110032)
    void cdbg_start_logging();
    void cdbg_stop_logging();
```

Edit `mainwindow.h` — in the slots section, after `void mitsubishi_dma_poll();` (around line 402):

```cpp
    void mitsubishi_dma_poll();
```

add:

```cpp
    void cdbg_poll();
```

- [ ] **Step 2: Write log_operations_mitsubishi_cdbg.cpp**

Create `log_operations_mitsubishi_cdbg.cpp`:

```cpp
#include "mainwindow.h"
#include <QRegularExpression>
#include "protocol/mitsu_colt_can_cdbg_driver.h"
#include "protocol/fastecu_can_transport.h"
#include "serial_port_actions.h"

using namespace MitsuColtCanCdbg;

// Build the Cdbg channel list from the enabled CDBG log values, in the same
// lower_panel order the SSM/MUT_DMA display paths consume them, so streamed
// values align with the display. outIndices[i] is the index j into
// logValues->log_value_* for the channel returned at position i.
static QVector<CdbgChannel> cdbg_channels_from_logvalues(FileActions::LogValuesStructure* lv,
                                                          QVector<int>& outIndices)
{
    QVector<CdbgChannel> ch;
    outIndices.clear();
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); i++)
    {
        for (int j = 0; j < lv->log_value_id.length(); j++)
        {
            if (lv->lower_panel_log_value_id.at(i) == lv->log_value_id.at(j)
                && lv->log_value_protocol.at(j) == "CDBG"
                && lv->log_value_enabled.at(j) == "1")
            {
                CdbgChannel c;
                c.pointer = lv->log_value_address.at(j).toUInt(nullptr, 16);
                c.size = quint8(lv->log_value_length.at(j).toUInt());
                ch.append(c);
                outIndices.append(j);
            }
        }
    }
    return ch;
}

void MainWindow::cdbg_start_logging()
{
    serial->set_is_iso14230_connection(false);
    serial->set_add_iso14230_header(false);
    serial->set_is_can_connection(true);
    serial->set_is_iso15765_connection(false);
    serial->set_is_29_bit_id(false);
    serial->set_can_speed("500000");
    serial->set_can_destination_address(kReplyCanId);
    serial->open_serial_port();

    cdbg_transport = new cdbg::FastEcuCanTransport(serial);
    cdbg_driver = new CdbgLogDriver(*cdbg_transport);

    QVector<int> idx;
    QVector<CdbgChannel> ch = cdbg_channels_from_logvalues(logValues, idx);
    if (!cdbg_driver->startFreeFormLog(ch))
    {
        emit LOG_E("Cdbg: session/security handshake failed", true, true);
        cdbg_stop_logging();
        return;
    }

    connect(logparams_poll_timer, SIGNAL(timeout()), this, SLOT(cdbg_poll()));
    logparams_poll_timer->start(10);
}

void MainWindow::cdbg_poll()
{
    if (!cdbg_driver || !cdbg_driver->isStreaming())
        return;

    QVector<quint32> vals = cdbg_driver->pollOnce(20);
    if (vals.isEmpty())
        return;

    QVector<int> idx;
    QVector<CdbgChannel> ch = cdbg_channels_from_logvalues(logValues, idx);

    // Conversion mirrors mitsubishi_dma_poll() in log_operations_mitsubishi.cpp
    // exactly: each raw channel value is rendered as one decimal integer and
    // fed through the RomRaider-format from_byte expression for that channel.
    for (int i = 0; i < vals.size() && i < ch.size(); ++i)
    {
        int j = idx.at(i);

        QStringList conversion = logValues->log_value_units.at(j).split(",");
        QString from_byte = conversion.at(2);
        QStringList format_str_lst = conversion.at(3).split(".");
        uint8_t format = 0;
        if (format_str_lst.length() > 1)
            format = format_str_lst.at(1).count(QRegularExpression("0"));

        QString value = QString::number(vals.at(i));
        double calc_float_value = fileActions->calculate_value_from_expression(
            fileActions->parse_stringlist_from_expression_string(from_byte, value));
        QString calc_value = QString::number(calc_float_value, 'f', format);
        logValues->log_value.replace(j, calc_value);
    }
}

void MainWindow::cdbg_stop_logging()
{
    if (logparams_poll_timer)
        logparams_poll_timer->stop();
    disconnect(logparams_poll_timer, SIGNAL(timeout()), this, SLOT(cdbg_poll()));

    delete cdbg_driver;    cdbg_driver = nullptr;
    delete cdbg_transport; cdbg_transport = nullptr;
}
```

- [ ] **Step 3: Dispatch from toggle_realtime()**

Edit `menu_actions.cpp` — the start branch currently reads (around line 924):

```cpp
        logging_counter = 0;
        logging_state = true;
        if (configValues->flash_protocol_selected_log_protocol == "MUT_DMA")
        {
            mitsubishi_dma_start_logging();
        }
        else
        {
            log_ssm_values();
            logparams_poll_timer->start();
        }
```

Change to:

```cpp
        logging_counter = 0;
        logging_state = true;
        if (configValues->flash_protocol_selected_log_protocol == "MUT_DMA")
        {
            mitsubishi_dma_start_logging();
        }
        else if (configValues->flash_protocol_selected_log_protocol == "CDBG")
        {
            cdbg_start_logging();
        }
        else
        {
            log_ssm_values();
            logparams_poll_timer->start();
        }
```

The stop branch currently reads (around line 944):

```cpp
        logging_state = false;
        log_params_request_started = false;
        if (configValues->flash_protocol_selected_log_protocol == "MUT_DMA")
        {
            mitsubishi_dma_stop_logging();
        }
        else
        {
            log_ssm_values();
            delay(200);
            logparams_poll_timer->stop();
        }
```

Change to:

```cpp
        logging_state = false;
        log_params_request_started = false;
        if (configValues->flash_protocol_selected_log_protocol == "MUT_DMA")
        {
            mitsubishi_dma_stop_logging();
        }
        else if (configValues->flash_protocol_selected_log_protocol == "CDBG")
        {
            cdbg_stop_logging();
        }
        else
        {
            log_ssm_values();
            delay(200);
            logparams_poll_timer->stop();
        }
```

- [ ] **Step 4: Handle Cdbg in the MainWindow destructor's logging cleanup**

Edit `mainwindow.cpp` — the destructor currently reads (around line 492):

```cpp
MainWindow::~MainWindow()
{
    if (logging_state)
    {
        logging_state = false;
        log_params_request_started = false;
        if (configValues->flash_protocol_selected_log_protocol == "MUT_DMA")
        {
            mitsubishi_dma_stop_logging();
        }
        else
        {
            log_ssm_values();
            delay(200);
        }
    }
    //ssm_init_poll_timer->stop();
    //serial_poll_timer->stop();
    delete ui;
}
```

Change to:

```cpp
MainWindow::~MainWindow()
{
    if (logging_state)
    {
        logging_state = false;
        log_params_request_started = false;
        if (configValues->flash_protocol_selected_log_protocol == "MUT_DMA")
        {
            mitsubishi_dma_stop_logging();
        }
        else if (configValues->flash_protocol_selected_log_protocol == "CDBG")
        {
            cdbg_stop_logging();
        }
        else
        {
            log_ssm_values();
            delay(200);
        }
    }
    //ssm_init_poll_timer->stop();
    //serial_poll_timer->stop();
    delete ui;
}
```

- [ ] **Step 5: Wire the new source file into FastECU.pro**

Edit `FastECU.pro` — the `SOURCES` list currently has (around line 101-102):

```
    log_operations_ssm.cpp \
    log_operations_mitsubishi.cpp \
```

Change to:

```
    log_operations_ssm.cpp \
    log_operations_mitsubishi.cpp \
    log_operations_mitsubishi_cdbg.cpp \
```

- [ ] **Step 6: Build the full application to verify it compiles**

Run: `qmake6 FastECU.pro && make -j4`
Expected: the build completes without error (or with only pre-existing warnings unrelated to these changes). This is the verification step for this task — there is no automated test for `MainWindow` integration code, matching the existing MUT/DMA precedent.

- [ ] **Step 7: Commit**

```bash
git add log_operations_mitsubishi_cdbg.cpp mainwindow.h mainwindow.cpp menu_actions.cpp FastECU.pro
git commit -m "feat: wire Cdbg CAN logging into FastECU's Logging screen"
```

---

### Task 6: Config wiring and bench checklist

**Files:**
- Modify: `config/protocols.cfg`
- Create: `config/logger_cdbg_example.xml`
- Create: `docs/cdbg-can-logging-bench-checklist.md`

**Interfaces:**
- Consumes: the `mitsu_ecu_m32r_can` protocol block in `config/protocols.cfg` (already exists, added by the CAN reflash port).
- Produces: `mitsu_ecu_m32r_can`'s `<log_protocol>` now `CDBG` (was `SSM`), `<log_transport>` now `CAN` (was `K-Line`); `config/logger_cdbg_example.xml` for the `"CDBG"` protocol id consumed by `FileActions::read_logger_definition_file()`.

- [ ] **Step 1: Update protocols.cfg**

Edit `config/protocols.cfg` — the `mitsu_ecu_m32r_can` block currently reads:

```xml
        <protocol name="mitsu_ecu_m32r_can">
            <ecu>Mitsubishi M32R</ecu>
            <mcu>M32R_384KB_1block</mcu>
            <mode>OBD2</mode>
            <checksum>no</checksum>
            <read>yes</read>
            <test_write>no</test_write>
            <write>yes</write>
            <flash_transport>iso15765</flash_transport>
            <log_transport>K-Line</log_transport>
            <log_protocol>SSM</log_protocol>
            <cal_id_ascii>no</cal_id_ascii>
            <cal_id_addr>0x0</cal_id_addr>
            <cal_id_length>0</cal_id_length>
            <description>Mitsubishi Colt CZT Z37A CAN (M32176F3/384KB, ROM 47110032)</description>
        </protocol>
```

Change the `log_transport` and `log_protocol` lines (these were placeholders left over from when this block only covered flash read/write, per `docs/superpowers/specs/2026-06-30-fastecu-colt-can-reflash-design.md`):

```xml
        <protocol name="mitsu_ecu_m32r_can">
            <ecu>Mitsubishi M32R</ecu>
            <mcu>M32R_384KB_1block</mcu>
            <mode>OBD2</mode>
            <checksum>no</checksum>
            <read>yes</read>
            <test_write>no</test_write>
            <write>yes</write>
            <flash_transport>iso15765</flash_transport>
            <log_transport>CAN</log_transport>
            <log_protocol>CDBG</log_protocol>
            <cal_id_ascii>no</cal_id_ascii>
            <cal_id_addr>0x0</cal_id_addr>
            <cal_id_length>0</cal_id_length>
            <description>Mitsubishi Colt CZT Z37A CAN (M32176F3/384KB, ROM 47110032)</description>
        </protocol>
```

- [ ] **Step 2: Create the example Cdbg logger definition**

Create `config/logger_cdbg_example.xml`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!--
  Example RomRaider-format logger definition for the Mitsubishi Cdbg CAN
  logging protocol (ROM 47110032, Colt CZT Z37A).

  This is the file format consumed by FileActions::read_logger_definition_file()
  (the file selected as "romraider_logger_definition_file" in preferences). It is
  NOT config/logger.cfg - that file only stores which parameter IDs are selected
  per ECU, it does not carry addresses/lengths/conversions.

  The two parameters below are PLACEHOLDERS, deliberately fake round-number
  addresses - NOT bench-confirmed 47110032 RAM addresses. Do not substitute
  livemonitor's schema.xml/czt_schema.xml pointer values here either: those
  were reverse-engineered against an uncertain/possibly-different ROM
  revision. Real channels must come from rom-analyzer run against 47110032,
  confirmed on a bench ECU, per docs/cdbg-can-logging-bench-checklist.md.
-->
<logger version="experimental">
    <protocols>
        <protocol id="CDBG">
            <parameters>
                <parameter id="C1" name="CDBG EXAMPLE A - replace with real 47110032 address" desc="Placeholder 1-byte channel" length="1">
                    <address>0x804000</address>
                    <conversions>
                        <conversion units="raw" expr="x" format="0" gauge_min="0" gauge_max="255" gauge_step="16"/>
                    </conversions>
                </parameter>
                <parameter id="C2" name="CDBG EXAMPLE B - replace with real 47110032 address" desc="Placeholder 2-byte channel" length="2">
                    <address>0x804010</address>
                    <conversions>
                        <conversion units="raw" expr="x" format="0" gauge_min="0" gauge_max="65535" gauge_step="1024"/>
                    </conversions>
                </parameter>
            </parameters>
        </protocol>
    </protocols>
</logger>
```

- [ ] **Step 3: Verify both XML files are well-formed**

Run: `python3 -c "import xml.dom.minidom as m; m.parse('config/protocols.cfg'); print('protocols.cfg OK')"`
Expected: `protocols.cfg OK`

Run: `python3 -c "import xml.dom.minidom as m; m.parse('config/logger_cdbg_example.xml'); print('logger_cdbg_example.xml OK')"`
Expected: `logger_cdbg_example.xml OK`

- [ ] **Step 4: Write the bench qualification checklist**

Create `docs/cdbg-can-logging-bench-checklist.md`:

```markdown
# Colt CZT (Z37A, 47110032) Cdbg CAN logging — bench qualification checklist

Gate before any real-vehicle use of Cdbg live-data logging (`log_protocol`
`CDBG`, `MitsuColtCanCdbg::CdbgLogDriver`). Same convention as this
project's other bench-qualification gates (e.g.
`docs/colt_czt_47110032_can_bench_checklist.md` for the CAN reflash port).

1. **Raw CAN connect.** Power a bench/spare Z37A ECU, select "Mitsubishi /
   Colt CZT / Z37A 5MT" in FastECU, enable "Logging". Confirm the raw CAN
   connection opens (`set_is_can_connection(true)`, destination filter
   `0x631`) without error.
2. **Session handshake.** Confirm `CdbgLogDriver::startFreeFormLog()`'s
   init command gets a reply on `0x631` at all. If there is no reply, the
   "stock/built-in feature" assumption behind this port is wrong - stop
   and revisit before investing further here.
3. **Security access.** Confirm the seed/key exchange grants access
   (security-flags byte nonzero in the key-response reply).
4. **Streaming.** With the placeholder channels from
   `config/logger_cdbg_example.xml`, confirm frames arrive on `0x631` at
   the configured interval and decode to *some* value (even if the
   placeholder address doesn't mean anything meaningful yet).
5. **Only after 1-4 pass repeatably**, invest in deriving real channel
   addresses via `rom-analyzer` against `47110032` for a first useful
   default channel set - a follow-up, not part of the initial port.
```

- [ ] **Step 5: Commit**

```bash
git add config/protocols.cfg config/logger_cdbg_example.xml docs/cdbg-can-logging-bench-checklist.md
git commit -m "feat: wire CDBG log protocol into protocols.cfg, add example logger def and bench checklist"
```
