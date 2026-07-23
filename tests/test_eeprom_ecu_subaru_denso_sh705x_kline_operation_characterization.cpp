// Characterization tests for EepromEcuSubaruDensoSH705xKlineOperation
// (src/backend/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_kline_operation.cpp).
//
// Purpose (step 5c, Task 6): this legacy 1,010-line QThread-based operation
// class has no focused tests today and is slated for deletion once Task 8's
// portable DensoSh705xEepromKlineExecutor replaces it. Every literal byte
// sequence built by the helpers below is transcribed from a specific line
// range in the real .cpp (cited in each helper's comment) -- nothing here is
// invented. Where a value is runtime-computed in production (the seed key,
// the encrypted kernel payload), the test calls the exact same SsmProtocol
// helper with the exact same tables the production code uses, rather than
// hardcoding a value that could not be independently verified.
//
// Harness pattern follows tests/test_flash_ecu_mitsu_m32r_can_operation.cpp:
// a real SerialPortActions constructed with the backendFactoryForTests hook
// returning a FakeBackend/QueuedFakeBackend, the real operation class driven
// via QThread::start(), and exact byte-sequence assertions via
// fake->takeCallLog().

#include "src/backend/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_kline_operation.h"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QMessageBox>
#include <QQueue>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryFile>
#include <QTest>
#include <QWidget>

#include "fake_backend.h"
#include "src/algorithms/protocol/ssm/ssm_protocol.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/definitions/kernelcomms.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "test_eeprom_ecu_subaru_denso_sh705x_kline_operation_characterization.h"

class QueuedFakeBackend : public FakeBackend
{
    Q_OBJECT
  public:
    QQueue<QByteArray> responses;

    QByteArray read_serial_data(uint16_t timeout) override
    {
        Q_UNUSED(timeout)
        if (responses.isEmpty())
        {
            return QByteArray();
        }
        return responses.dequeue();
    }
};

namespace
{

// eeprom_ecu_subaru_denso_sh705x_kline_operation.cpp:62-63 --
// execute() hardcodes tester_id = 0xF0, target_id = 0x10 before ever calling
// connect_bootloader()/upload_kernel(); every send_sid_* helper frames its
// request via SsmProtocol::addHeader(output, tester_id, target_id, false).
constexpr quint8 kTesterId = 0xF0;
constexpr quint8 kTargetId = 0x10;

QString hex(const QByteArray& b)
{
    return QString::fromLatin1(b.toHex());
}

// ---- Request builders: TRANSCRIBE of each send_sid_* body ----------------

// send_sid_bf_ssm_init(), eeprom_ecu_subaru_denso_sh705x_kline_operation.cpp:609-621:
// output = [0xBF], framed via SsmProtocol::addHeader.
QByteArray sidBfSsmInitRequest()
{
    QByteArray out;
    out.append(char(0xBF));
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}

// send_sid_81_start_communication(), lines 628-640: output = [0x81].
QByteArray sid81StartCommRequest()
{
    QByteArray out;
    out.append(char(0x81));
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}

// send_sid_83_request_timings(), lines 647-660: output = [0x83, 0x00].
QByteArray sid83TimingsRequest()
{
    QByteArray out;
    out.append(char(0x83));
    out.append(char(0x00));
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}

// send_sid_27_request_seed(), lines 667-680: output = [0x27, 0x01].
QByteArray sid27RequestSeedRequest()
{
    QByteArray out;
    out.append(char(0x27));
    out.append(char(0x01));
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}

// send_sid_27_send_seed_key(), lines 687-701: output = [0x27, 0x02] + seed_key (4 bytes).
QByteArray sid27SendKeyRequest(const QByteArray& key)
{
    QByteArray out;
    out.append(char(0x27));
    out.append(char(0x02));
    out.append(key);
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}

// send_sid_10_start_diagnostic(), lines 708-722: output = [0x10, 0x85, 0x02].
QByteArray sid10StartDiagRequest()
{
    QByteArray out;
    out.append(char(0x10));
    out.append(char(0x85));
    out.append(char(0x02));
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}

// send_sid_34_request_upload(), lines 729-748:
// output = [0x34, addr>>16, addr>>8, addr, 0x04, len>>16, len>>8, len].
QByteArray sid34RequestUploadRequest(quint32 dataaddr, quint32 datalen)
{
    QByteArray out;
    out.append(char(0x34));
    out.append(char((dataaddr >> 16) & 0xFF));
    out.append(char((dataaddr >> 8) & 0xFF));
    out.append(char(dataaddr & 0xFF));
    out.append(char(0x04));
    out.append(char((datalen >> 16) & 0xFF));
    out.append(char((datalen >> 8) & 0xFF));
    out.append(char(datalen & 0xFF));
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}

// send_sid_36_transferdata(), lines 755-831: per-block output =
// [0x36, blockaddr>>16, blockaddr>>8, blockaddr] + up to 0x80 (128) bytes of
// (already-encrypted) payload; the *last* (or only) block appends whatever
// remains of `len` instead of a full 128-byte chunk (lines 789-803). This
// helper only covers the single-block case (len <= 128), which is all this
// test's fixtures need.
QByteArray sid36TransferDataRequest(quint32 blockaddr, const QByteArray& blockBytes)
{
    QByteArray out;
    out.append(char(0x36));
    out.append(char((blockaddr >> 16) & 0xFF));
    out.append(char((blockaddr >> 8) & 0xFF));
    out.append(char(blockaddr & 0xFF));
    out.append(blockBytes);
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}

// send_sid_31_start_routine(), lines 833-847: output = [0x31, 0x01, 0x01].
QByteArray sid31StartRoutineRequest()
{
    QByteArray out;
    out.append(char(0x31));
    out.append(char(0x01));
    out.append(char(0x01));
    return SsmProtocol::addHeader(out, kTesterId, kTargetId, false);
}

// request_kernel_id(), lines 964-994: NOT SsmProtocol::addHeader-framed --
// output = [SUB_KERNEL_START_COMM>>8, SUB_KERNEL_START_COMM&0xFF,
//           (datalen+1)>>8, (datalen+1)&0xFF, SUB_KERNEL_ID, checksum]
// with datalen == 0 hardcoded (line 974), so (datalen+1) == 1.
QByteArray requestKernelIdRequest()
{
    QByteArray out;
    out.append(char((SUB_KERNEL_START_COMM >> 8) & 0xFF));
    out.append(char(SUB_KERNEL_START_COMM & 0xFF));
    out.append(char(((0 + 1) >> 8) & 0xFF));
    out.append(char((0 + 1) & 0xFF));
    out.append(char(SUB_KERNEL_ID & 0xFF));
    out.append(char(SsmProtocol::checksum(out, false)));
    return out;
}

// generate_seed_key(), lines 854-879: SsmProtocol::calculateSeedKey with this
// exact table pair (the non-ecutek variant -- flash_method not ending in
// "_ecutek" is what our fixtures exercise, matching FlashMethod left blank).
QByteArray generateSeedKey(const QByteArray& seed)
{
    const uint16_t keytogenerateindex_1[] = {
        0x53DA, 0x33BC, 0x72EB, 0x437D,
        0x7CA3, 0x3382, 0x834F, 0x3608,
        0xAFB8, 0x503D, 0xDBA3, 0x9D34,
        0x3563, 0x6B70, 0x6E74, 0x88F0};
    const uint8_t indextransformation[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
        0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
        0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, keytogenerateindex_1, indextransformation);
}

// encrypt_payload(), lines 923-939: SsmProtocol::calculatePayload with this
// exact table pair.
QByteArray encryptPayload(const QByteArray& buf, uint32_t len)
{
    uint16_t keytogenerateindex[] = {0x7856, 0xCE22, 0xF513, 0x6E86};
    const uint8_t indextransformation[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
        0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
        0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculatePayload(buf, len, keytogenerateindex, indextransformation);
}

// read_mem(), lines 476-521: the SID_DUMP request is a raw, unframed write
// (nisprog-kernel protocol, distinct from every SSM-framed send_sid_*
// helper above -- no SsmProtocol::addHeader call site exists in read_mem()).
// output = [SID_DUMP, EEPROM_MODE, 0,0,0,0], then output[2..5] overwritten
// with numblocks (hi/lo) and curblock (hi/lo) (lines 480-520).
QByteArray sidDumpRequest(quint8 eepromMode, quint8 numblocksHi, quint8 numblocksLo,
                          quint8 curblockHi, quint8 curblockLo)
{
    QByteArray out;
    out.append(char(SID_DUMP));
    out.append(char(eepromMode));
    out.append(char(numblocksHi));
    out.append(char(numblocksLo));
    out.append(char(curblockHi));
    out.append(char(curblockLo));
    return out;
}

// For McuType "SH7055", flashdevices[].eblocks[0] == {start=0x00000000,
// len=0x00000100} (src/backend/definitions/kernelmemorymodels.h:279-281).
// read_mem()'s skip_start/willget/numblocks/curblock arithmetic (lines
// 469-510) reduces, for this start/length, to a single request with
// numblocks=8 (256/32) and curblock=0 -- this holds for every EEPROM_MODE
// value tested here, since the mode byte doesn't affect the block-count math.
QByteArray sidDumpRequestForSh7055(quint8 eepromMode)
{
    return sidDumpRequest(eepromMode, 0x00, 0x08, 0x00, 0x00);
}

// read_mem()'s per-block unframing (lines 545-550): each 35-byte wire block
// is [2 prefix bytes][32 data bytes][1 trailing byte]; the prefix and
// trailing bytes are stripped and never inspected by production code, so
// their value is arbitrary filler here. 8 blocks * 35 bytes = 280 bytes,
// matching pagesize = numblocks*32 + numblocks*3 for numblocks=8 (line 512).
QByteArray eepromPayload280Bytes()
{
    QByteArray out;
    for (int block = 0; block < 8; ++block)
    {
        out.append(char(0xEE)); // prefix byte 0 (stripped, unchecked)
        out.append(char(0xEE)); // prefix byte 1 (stripped, unchecked)
        for (int j = 0; j < 32; ++j)
        {
            out.append(char((block * 32 + j) & 0xFF));
        }
        out.append(char(0xFF)); // trailing byte (stripped, unchecked)
    }
    return out; // 280 bytes
}

// The 256-byte payload read_mem() should assemble into ecuCalDef->FullRomData
// once eepromPayload280Bytes()'s framing bytes are stripped: a 0x00.. 0xFF
// ramp, matching the data bytes threaded through eepromPayload280Bytes().
QByteArray expectedDecodedEeprom256Bytes()
{
    QByteArray out;
    for (int i = 0; i < 256; ++i)
    {
        out.append(char(i & 0xFF));
    }
    return out;
}

// Minimal positive-response fixture: only byte index 4 (the service/session
// code) is inspected by connect_bootloader()/upload_kernel() for these SIDs
// (lines 227, 237, 277, 287, 368, 412), so a 5-byte frame with that one byte
// set is sufficient and never ambiguous with the "" / length<=4 failure checks.
QByteArray positiveResponse(quint8 serviceCode)
{
    QByteArray out(5, char(0));
    out[4] = char(serviceCode);
    return out;
}

// send_sid_bf_ssm_init()'s response is checked more deeply than the others:
// connect_bootloader() (lines 204-219) requires byte[4] == 0xFF, then does
// `received.remove(0, 8); received.remove(5, received.length() - 5);` to
// extract a 5-byte "ecu id" -- needs >= 13 bytes total to avoid a negative
// remove() length. Content of the 8 header bytes and 5 id bytes is never
// asserted by this test (only logged), so it's arbitrary filler.
QByteArray sidBfSsmInitResponse()
{
    QByteArray out(13, char(0));
    out[4] = char(0xFF);
    out[8] = 'A';
    out[9] = 'B';
    out[10] = 'C';
    out[11] = 'D';
    out[12] = 'E';
    return out;
}

// send_sid_27_request_seed()'s response (lines 247-258): byte[4] == 0x67,
// and the 4-byte seed is read from byte offsets 6..9.
QByteArray sid27SeedResponse(const QByteArray& seed)
{
    QByteArray out(10, char(0));
    out[4] = char(0x67);
    out[6] = seed.at(0);
    out[7] = seed.at(1);
    out[8] = seed.at(2);
    out[9] = seed.at(3);
    return out;
}

// request_kernel_id()'s "kernel is alive" response (checked at
// connect_bootloader() lines 179-189 and the upload_kernel() poll loop at
// lines 429-441): received[0..1] == SUB_KERNEL_START_COMM (big-endian),
// received[4] == SUB_KERNEL_ID | 0x40. Trailing bytes (the "kernel id"
// string) are logged but not asserted.
QByteArray kernelAliveResponse()
{
    QByteArray out;
    out.append(char((SUB_KERNEL_START_COMM >> 8) & 0xFF));
    out.append(char(SUB_KERNEL_START_COMM & 0xFF));
    out.append(char(0x00));
    out.append(char(0x06));
    out.append(char(SUB_KERNEL_ID | 0x40));
    out.append("KERN2");
    return out; // 10 bytes
}

// A non-alive request_kernel_id() response: production only branches into
// the "kernel alive" fast path when received.length() > 4 (line 177); an
// empty response falls into the `else` branch (line 191-194), logs an
// error, and falls through to the normal bootloader-init sequence without
// returning early.
QByteArray kernelNotAliveResponse()
{
    return QByteArray();
}

QString writeKernelFixture(QTemporaryFile& file, const QByteArray& bytes)
{
    if (!file.open())
    {
        return {};
    }
    file.write(bytes);
    const QString path = file.fileName();
    file.close();
    return path;
}

// A 16-byte (already 4-byte-aligned) kernel fixture. upload_kernel() (line
// 345-346) rounds pl_len up to a multiple of 4 via `(pl_len + 3) & ~3`; since
// this fixture's length is already a multiple of 4, pl_len == the file's
// actual byte count and encrypt_payload() is called with matching buf/len,
// avoiding a latent out-of-bounds read in send_sid_36_transferdata() that a
// non-4-aligned kernel file would trigger (see this task's report).
QByteArray kernelFixtureBytes()
{
    QByteArray out;
    for (int i = 0; i < 16; ++i)
    {
        out.append(char(i));
    }
    return out;
}

// Enqueues the exact response sequence for one full "kernel not yet
// running" round: connect_bootloader()'s non-alive probe + full bf/81/83/
// 27/27/10 init, then upload_kernel()'s 34/36/34/36/31 sequence and its
// kernel-alive re-poll. 13 responses, matching the 13 writes this round
// produces (request_kernel_id, bf, 81, 83, 27req, 27key, 10, 34, 36, 34, 36,
// 31, request_kernel_id).
void enqueueFullBootloaderAndKernelUpload(QueuedFakeBackend *fake, const QByteArray& seed)
{
    fake->responses.enqueue(kernelNotAliveResponse());
    fake->responses.enqueue(sidBfSsmInitResponse());
    fake->responses.enqueue(positiveResponse(0xC1));
    fake->responses.enqueue(positiveResponse(0xC3));
    fake->responses.enqueue(sid27SeedResponse(seed));
    fake->responses.enqueue(positiveResponse(0x67));
    fake->responses.enqueue(positiveResponse(0x50));
    fake->responses.enqueue(positiveResponse(0x74));
    fake->responses.enqueue(positiveResponse(0x76));
    fake->responses.enqueue(positiveResponse(0x74));
    fake->responses.enqueue(positiveResponse(0x76));
    fake->responses.enqueue(positiveResponse(0x71));
    fake->responses.enqueue(kernelAliveResponse());
}

// Computes the exact ordered write sequence enqueueFullBootloaderAndKernelUpload's
// round should produce, using the real production tables/helpers for the
// seed-key and kernel-payload encryption (never hardcoded).
QStringList expectedFullBootloaderAndKernelUploadWrites(const QByteArray& seed, quint32 kernelStartAddr,
                                                        const QByteArray& kernelBytes)
{
    QStringList out;
    auto add = [&out](const QByteArray& request)
    { out << "write_echo_check:begin:" + hex(request); };

    add(requestKernelIdRequest());
    add(sidBfSsmInitRequest());
    add(sid81StartCommRequest());
    add(sid83TimingsRequest());
    add(sid27RequestSeedRequest());
    add(sid27SendKeyRequest(generateSeedKey(seed)));
    add(sid10StartDiagRequest());

    const quint32 pl_len = (quint32(kernelBytes.size()) + 3) & ~3u;
    add(sid34RequestUploadRequest(kernelStartAddr, pl_len));
    add(sid36TransferDataRequest(kernelStartAddr, encryptPayload(kernelBytes, pl_len)));

    QByteArray cksBypass;
    cksBypass.append(char(0x00));
    cksBypass.append(char(0x00));
    cksBypass.append(char(0x5A));
    cksBypass.append(char(0xA5));
    add(sid34RequestUploadRequest(kernelStartAddr + pl_len, 4));
    add(sid36TransferDataRequest(kernelStartAddr + pl_len, encryptPayload(cksBypass, 4)));

    add(sid31StartRoutineRequest());
    add(requestKernelIdRequest());
    return out;
}

FileActions::EcuCalDefStructure makeEcuCalDef()
{
    FileActions::EcuCalDefStructure def;
    def.McuType = "SH7055";
    // Matches resources/shared/config/protocols.cfg's
    // sub_ecu_eeprom_denso_sh7055_kline entry (kernel_addr = 0xFFFF6004).
    def.KernelStartAddr = "FFFF6004";
    return def;
}

constexpr quint32 kKernelStartAddr = 0xFFFF6004;

class SaveOnFirstPrompt
{
  public:
    static FlashOperationWorker::PromptFn make()
    {
        return [](QWidget *, const QString&, const QString&, int, int) -> int
        { return QMessageBox::Save; };
    }
};

class DiscardThenIgnitionOk
{
  public:
    static FlashOperationWorker::PromptFn make(int *callCount)
    {
        return [callCount](QWidget *, const QString&, const QString&, int, int) -> int
        {
            ++*callCount;
            // Odd calls are the "Save/Ignore downloaded content" prompt,
            // even calls are the "cycle ignition" prompt.
            return (*callCount % 2 == 1) ? QMessageBox::Ignore : QMessageBox::Ok;
        };
    }
};

} // namespace

class TestEepromKlineCharacterization : public QObject
{
    Q_OBJECT

  private slots:

    void kernelAlreadyRunning_skipsBootloaderAndReadsMode2()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false); // instantiates the backend

        fake->responses.enqueue(kernelAliveResponse());
        fake->responses.enqueue(eepromPayload280Bytes());

        auto ecuCalDef = makeEcuCalDef();
        QWidget dialog;
        EepromEcuSubaruDensoSH705xKlineOperation op(&serial, &ecuCalDef, "read", &dialog,
                                                    nullptr, SaveOnFirstPrompt::make());
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(5000));
        QVERIFY(op.wait(2000));

        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);

        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        const QStringList expected = {
            "write_echo_check:begin:" + hex(requestKernelIdRequest()),
            "write_echo_check:begin:" + hex(sidDumpRequestForSh7055(2)),
        };
        QCOMPARE(writes, expected);
        QCOMPARE(ecuCalDef.FullRomData, expectedDecodedEeprom256Bytes());
    }

    void fullBootloaderStockSecurity_mode2_saveEndsTheLoop()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        const QByteArray seed = QByteArray::fromHex("11223344");
        enqueueFullBootloaderAndKernelUpload(fake, seed);
        fake->responses.enqueue(eepromPayload280Bytes());

        QTemporaryFile kernelFile;
        auto ecuCalDef = makeEcuCalDef();
        ecuCalDef.Kernel = writeKernelFixture(kernelFile, kernelFixtureBytes());
        QVERIFY(!ecuCalDef.Kernel.isEmpty());

        int promptCalls = 0;
        auto prompt = [&promptCalls](QWidget *, const QString&, const QString&, int, int) -> int
        {
            ++promptCalls;
            return QMessageBox::Save;
        };

        QWidget dialog;
        EepromEcuSubaruDensoSH705xKlineOperation op(&serial, &ecuCalDef, "read", &dialog, nullptr, prompt);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(8000));
        QVERIFY(op.wait(2000));

        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        QStringList expected = expectedFullBootloaderAndKernelUploadWrites(seed, kKernelStartAddr,
                                                                           kernelFixtureBytes());
        expected << "write_echo_check:begin:" + hex(sidDumpRequestForSh7055(2));
        QCOMPARE(writes, expected);

        QCOMPARE(promptCalls, 1); // no ignition-cycle prompt -- EEPROM_MODE never advanced past 2
        QCOMPARE(ecuCalDef.FullRomData, expectedDecodedEeprom256Bytes());
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
    }

    void mode2Discard_thenMode3Discard_thenMode4AllDiscarded_endsUnsuccessful()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        const QByteArray seed = QByteArray::fromHex("11223344");
        QTemporaryFile kernelFile;
        auto ecuCalDef = makeEcuCalDef();
        ecuCalDef.Kernel = writeKernelFixture(kernelFile, kernelFixtureBytes());
        QVERIFY(!ecuCalDef.Kernel.isEmpty());

        // Round 1 (EEPROM_MODE == 2): kernel not yet running -- full init +
        // kernel upload, then the mode-2 EEPROM read.
        enqueueFullBootloaderAndKernelUpload(fake, seed);
        fake->responses.enqueue(eepromPayload280Bytes());
        // Round 2 (EEPROM_MODE == 3): kernel_alive is now true, so
        // connect_bootloader()'s request_kernel_id() alone succeeds and
        // upload_kernel() is skipped entirely -- straight to the read.
        fake->responses.enqueue(kernelAliveResponse());
        fake->responses.enqueue(eepromPayload280Bytes());
        // Round 3 (EEPROM_MODE == 4): same shape as round 2.
        fake->responses.enqueue(kernelAliveResponse());
        fake->responses.enqueue(eepromPayload280Bytes());

        int promptCalls = 0;
        QWidget dialog;
        EepromEcuSubaruDensoSH705xKlineOperation op(&serial, &ecuCalDef, "read", &dialog, nullptr,
                                                    DiscardThenIgnitionOk::make(&promptCalls));
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(10000));
        QVERIFY(op.wait(2000));

        const QStringList dumpWrites = fake->takeCallLog().filter("write_echo_check:begin:bd");
        const QStringList expectedDumps = {
            "write_echo_check:begin:" + hex(sidDumpRequestForSh7055(2)),
            "write_echo_check:begin:" + hex(sidDumpRequestForSh7055(3)),
            "write_echo_check:begin:" + hex(sidDumpRequestForSh7055(4)),
        };
        QCOMPARE(dumpWrites, expectedDumps); // literal proof EEPROM_MODE progresses 2 -> 3 -> 4, never repeats/skips

        QCOMPARE(promptCalls, 6);                          // 3 rounds * (discard prompt + ignition-cycle prompt)
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false); // no round saved -- loop exhausts naturally after mode 4
    }

    void ignitionCyclePromptCancel_endsTheLoopWithoutAFourthRound()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        const QByteArray seed = QByteArray::fromHex("11223344");
        QTemporaryFile kernelFile;
        auto ecuCalDef = makeEcuCalDef();
        ecuCalDef.Kernel = writeKernelFixture(kernelFile, kernelFixtureBytes());
        QVERIFY(!ecuCalDef.Kernel.isEmpty());

        enqueueFullBootloaderAndKernelUpload(fake, seed);
        fake->responses.enqueue(eepromPayload280Bytes());

        int promptCalls = 0;
        auto prompt = [&promptCalls](QWidget *, const QString&, const QString&, int, int) -> int
        {
            ++promptCalls;
            // First prompt (Save/Ignore downloaded content) -> discard.
            // Second prompt (cycle ignition) -> Cancel.
            return (promptCalls == 1) ? QMessageBox::Ignore : QMessageBox::Cancel;
        };

        QWidget dialog;
        EepromEcuSubaruDensoSH705xKlineOperation op(&serial, &ecuCalDef, "read", &dialog, nullptr, prompt);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(8000));
        QVERIFY(op.wait(2000));

        const QStringList dumpWrites = fake->takeCallLog().filter("write_echo_check:begin:bd");
        QCOMPARE(dumpWrites, QStringList{"write_echo_check:begin:" + hex(sidDumpRequestForSh7055(2))});

        QCOMPARE(promptCalls, 2); // discard, then Cancel on the ignition-cycle prompt -- no round 2
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
    }

    void testWriteCommand_uploadsKernelButNeverWritesEeprom()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        const QByteArray seed = QByteArray::fromHex("11223344");
        enqueueFullBootloaderAndKernelUpload(fake, seed);
        // No further response queued: cmd_type == "test_write" never reaches
        // read_mem() (eeprom_ecu_subaru_denso_sh705x_kline_operation.cpp:82-93
        // -- the write_mem() call is commented out and `result` is left
        // untouched from upload_kernel()'s STATUS_SUCCESS), so no SID_DUMP
        // write should ever occur.

        QTemporaryFile kernelFile;
        auto ecuCalDef = makeEcuCalDef();
        ecuCalDef.Kernel = writeKernelFixture(kernelFile, kernelFixtureBytes());
        QVERIFY(!ecuCalDef.Kernel.isEmpty());

        QWidget dialog;
        EepromEcuSubaruDensoSH705xKlineOperation op(&serial, &ecuCalDef, "test_write", &dialog, nullptr,
                                                    SaveOnFirstPrompt::make());
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(8000));
        QVERIFY(op.wait(2000));

        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        const QStringList expected = expectedFullBootloaderAndKernelUploadWrites(seed, kKernelStartAddr,
                                                                                 kernelFixtureBytes());
        QCOMPARE(writes, expected); // upload_kernel()'s writes occur; no trailing SID_DUMP write
        // Anchored on the log-entry prefix (not a bare "bd" substring search,
        // which could false-positive on an encrypted payload byte that
        // happens to contain "bd"): no request's hex payload starts with bd.
        QVERIFY(writes.filter("write_echo_check:begin:bd").isEmpty());

        // Legacy gap this pins: the operation reports success despite never
        // writing anything to the ECU's EEPROM.
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
    }

    void writeCommand_uploadsKernelButNeverWritesEeprom()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        const QByteArray seed = QByteArray::fromHex("11223344");
        enqueueFullBootloaderAndKernelUpload(fake, seed);

        QTemporaryFile kernelFile;
        auto ecuCalDef = makeEcuCalDef();
        ecuCalDef.Kernel = writeKernelFixture(kernelFile, kernelFixtureBytes());
        QVERIFY(!ecuCalDef.Kernel.isEmpty());

        QWidget dialog;
        EepromEcuSubaruDensoSH705xKlineOperation op(&serial, &ecuCalDef, "write", &dialog, nullptr,
                                                    SaveOnFirstPrompt::make());
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(8000));
        QVERIFY(op.wait(2000));

        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        const QStringList expected = expectedFullBootloaderAndKernelUploadWrites(seed, kKernelStartAddr,
                                                                                 kernelFixtureBytes());
        QCOMPARE(writes, expected);
        QVERIFY(writes.filter("write_echo_check:begin:bd").isEmpty());
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true); // same legacy gap as "test_write"
    }

    void protocolsCfgDeclaresEepromWriteAndTestWriteUnsupported()
    {
        const QByteArray envPath = qgetenv("PROTOCOLS_CFG_PATH");
        QVERIFY2(!envPath.isEmpty(),
                 "PROTOCOLS_CFG_PATH not set -- see bazel/mut_dma_test_suites.bzl's data/env "
                 "wiring for this suite");

        QFile cfg(QString::fromLocal8Bit(envPath));
        QVERIFY(cfg.open(QIODevice::ReadOnly));
        const QString contents = QString::fromUtf8(cfg.readAll());

        for (const QString& name : {
                 "sub_ecu_eeprom_denso_sh7055_kline", "sub_ecu_eeprom_denso_sh7058_kline",
                 "sub_ecu_eeprom_denso_sh7055_densocan", "sub_ecu_eeprom_denso_sh7058_densocan",
                 "sub_ecu_eeprom_denso_sh7058_can", "sub_ecu_eeprom_denso_sh7058_can_diesel"})
        {
            const int nameIndex = contents.indexOf(QString("name=\"%1\"").arg(name));
            QVERIFY2(nameIndex >= 0, qPrintable(name));
            const int entryEnd = contents.indexOf("</protocol>", nameIndex);
            const QString entry = contents.mid(nameIndex, entryEnd - nameIndex);
            QVERIFY2(entry.contains("<test_write>no</test_write>"), qPrintable(name));
            QVERIFY2(entry.contains("<write>no</write>"), qPrintable(name));
        }
    }
};

int run_test_eeprom_ecu_subaru_denso_sh705x_kline_operation_characterization(int argc, char **argv)
{
    // EepromEcuSubaruDensoSH705xKlineOperation's constructor takes a QWidget*
    // dialog, and the tests below construct a real QWidget -- that requires a
    // QApplication rather than a plain QCoreApplication (same fix as
    // test_flash_ecu_mitsu_m32r_can_operation.cpp's run function).
    QApplication app(argc, argv);
    TestEepromKlineCharacterization t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_eeprom_ecu_subaru_denso_sh705x_kline_operation_characterization.moc"
