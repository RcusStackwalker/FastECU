// Equivalence + error-matrix tests for DensoSh705xEepromCanExecutor, the
// portable replacement for the deleted EepromEcuSubaruDensoSH705xCanOperation.
// Every literal byte sequence below is either transcribed directly from the
// legacy .cpp (see the comments citing exact line numbers, matching
// task-7-report.md's table) or computed at runtime via the same SsmProtocol
// helpers and hardcoded tables production used, matching the now-deleted
// characterization test's own approach (tests/test_eeprom_ecu_subaru_denso_
// sh705x_can_operation_characterization.cpp, commit 3eed21a) -- nothing here
// is invented.
//
// The legacy "_ecutek_racerom_alt" flash-method branch (and its
// read_ram_location() RAM-preprocessing step) has no equivalent here: see
// the long comment above DensoSh705xEepromCanExecutor::connect_bootloader()
// in the .cpp for why this is an intentional, out-of-scope resolution of
// Task 4's OPEN QUESTION, not an oversight.
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string_view>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"

using ::testing::ElementsAre;

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::u24;
using namespace bytes::literals;

// eeprom_ecu_subaru_denso_sh705x_can_operation.cpp's serial->set_can_source_
// address(0x7e0)/set_iso15765_destination_address(0x7e8) (lines 61-64),
// mirrored by the builder's DensoSh705xEepromCanPlan.
constexpr std::uint32_t kRequestId = 0x7e0;

// Matches resources/shared/config/protocols.cfg's CAN protocol entries'
// kernel_addr for McuType "SH7055" (kblocks_SH7055[0].start), the same
// literal the K-Line sibling's test uses -- Task 7's own CAN characterization
// test used this exact McuType/address pair too (see its makeEcuCalDef()).
constexpr std::uint32_t kKernelStartAddr = 0xFFFF6004;

// ---- Request builders: TRANSCRIBE of every inline UDS-over-CAN frame -----
// Every non-read_ram_location() request is built inline in
// connect_bootloader()/upload_kernel()/read_mem() with a hardcoded 4-byte
// "CAN ID" prefix [0x00,0x00,0x07,0xE0] == kRequestId encoded big-endian.
// Kept as a literal here (not derived from kRequestId) to independently
// verify the executor derives it correctly from the plan rather than also
// hardcoding it.

bytes::Bytes initConnectionRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x01, 0x00};
}
bytes::Bytes ecuIdRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0xAA};
}
bytes::Bytes vinRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x09, 0x02};
}
bytes::Bytes calIdRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x09, 0x04};
}
bytes::Bytes cvnRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x09, 0x06};
}
bytes::Bytes sessionMode03Request()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x10, 0x03};
}
bytes::Bytes sessionMode43Request()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x10, 0x43};
}
bytes::Bytes seedRequestFrame()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x27, 0x01};
}
bytes::Bytes seedKeySendRequest(bytes::ByteView key)
{
    return composeBe(bytes::Bytes{0x00, 0x00, 0x07, 0xE0}, 0x27_b, 0x02_b, key);
}
// Every fixture below gives positive responses to both prior session-mode
// requests, so both flags are true and both bytes are appended.
bytes::Bytes sessionSetRequestBothConnected()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x10, 0x02, 0x42};
}
// request_kernel_id(), lines 1355-1390: UNLIKE the K-Line sibling's
// request_kernel_id(), NOT checksum-terminated.
bytes::Bytes requestKernelIdRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
}
bytes::Bytes sid34RequestDownloadRequest(std::uint32_t startAddress, std::uint32_t dataLen)
{
    return composeBe(bytes::Bytes{0x00, 0x00, 0x07, 0xE0}, 0x34_b, 0x04_b, 0x33_b, u24(startAddress), u24(dataLen));
}
bytes::Bytes sidB6TransferBlockRequest(std::uint32_t blockAddr, bytes::ByteView payload)
{
    return composeBe(bytes::Bytes{0x00, 0x00, 0x07, 0xE0}, 0xB6_b, u24(blockAddr), payload);
}
bytes::Bytes sid37StartKernelRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x37};
}
bytes::Bytes sid31StartRoutineRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x31, 0x01, 0x02, 0x02, 0x02};
}

// Anchors three helpers against hardcoded wire bytes -- each became the same
// composeBe expression as its production counterpart in
// denso_sh705x_eeprom_can_executor.cpp, so a width bug in u24() or composeBe
// would move both sides together and hide behind a passing suite. CAN frames
// here carry no checksum, so this is a pure width/order check.
//
// The CAN ID prefix is kRequestId (0x7E0) encoded as a 4-byte big-endian
// std::uint32_t: [0x00, 0x00, 0x07, 0xE0].

// seedKeySendRequest({0x33, 0x44}): payload = [0x27, 0x02, 0x33, 0x44].
TEST(DensoSh705xEepromCanExecutorTest, SeedKeySendRequestMatchesHardcodedWireBytes)
{
    EXPECT_THAT(seedKeySendRequest(bytes::Bytes{0x33, 0x44}),
                ElementsAre(0x00, 0x00, 0x07, 0xE0, 0x27, 0x02, 0x33, 0x44));
}

// sid34RequestDownloadRequest(0x002000, 0x000040):
// payload = [0x34, 0x04, 0x33, u24(0x002000), u24(0x000040)]
//         = [0x34, 0x04, 0x33, 0x00, 0x20, 0x00, 0x00, 0x00, 0x40].
TEST(DensoSh705xEepromCanExecutorTest, Sid34RequestDownloadRequestMatchesHardcodedWireBytes)
{
    EXPECT_THAT(sid34RequestDownloadRequest(0x002000, 0x000040),
                ElementsAre(0x00, 0x00, 0x07, 0xE0, 0x34, 0x04, 0x33, 0x00, 0x20, 0x00, 0x00, 0x00, 0x40));
}

// sidB6TransferBlockRequest(0x003000, {0x55, 0x66, 0x77}):
// payload = [0xB6, u24(0x003000), 0x55, 0x66, 0x77]
//         = [0xB6, 0x00, 0x30, 0x00, 0x55, 0x66, 0x77].
TEST(DensoSh705xEepromCanExecutorTest, SidB6TransferBlockRequestMatchesHardcodedWireBytes)
{
    EXPECT_THAT(sidB6TransferBlockRequest(0x003000, bytes::Bytes{0x55, 0x66, 0x77}),
                ElementsAre(0x00, 0x00, 0x07, 0xE0, 0xB6, 0x00, 0x30, 0x00, 0x55, 0x66, 0x77));
}
// read_mem(), for McuType "SH7055" (eblocks_SH7055[0] == {start=0,
// len=0x100}): reduces to a single request with addr=0, pagesize=0x100.
bytes::Bytes sidReadEepromRequestForSh7055(std::uint8_t eepromMode)
{
    return {
        0x00,       0x00, 0x07, 0xe0, 0xBE, 0xEF, 0x00, 0x07,
        0x07, // SUB_KERNEL_READ_EEPROM
        eepromMode,
        0x00, // addr>>16 (addr == 0)
        0x00, // addr>>8
        0x00, // addr
        0x01, // pagesize>>8 (pagesize == 0x100)
        0x00, // pagesize
    };
}

// ---- Seed-key algorithms: TRANSCRIBE of each generate_*_seed_key() --------

bytes::Bytes generateSeedKeyStock(bytes::ByteView seed)
{
    static constexpr std::uint16_t kIndex[] = {0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
                                               0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};
    static constexpr std::uint8_t kTransform[] = {0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                  0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                  0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, kIndex, kTransform);
}
bytes::Bytes generateEcutekSeedKeyPlain(bytes::ByteView seed)
{
    static constexpr std::uint16_t kIndex[] = {0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
                                               0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};
    static constexpr std::uint8_t kTransform[] = {0x4, 0x2, 0x5, 0x1, 0x8, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                  0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                  0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, kIndex, kTransform);
}
bytes::Bytes generateCobbSeedKey(bytes::ByteView seed)
{
    static constexpr std::uint16_t kIndex[] = {0x9DDB, 0x9CFB, 0x9B9A, 0x6136, 0x59E1, 0xBA03, 0xD683, 0x7092,
                                               0x9E05, 0x8723, 0xF998, 0x15BB, 0xB8D5, 0xFF0C, 0x9D91, 0x24B9};
    static constexpr std::uint8_t kTransform[] = {0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                  0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                  0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, kIndex, kTransform);
}
std::uint64_t decryptRaceromSeed(std::uint64_t base, std::uint64_t exponent, std::uint64_t modulus)
{
    std::uint64_t result = 1;
    base = base % modulus;
    while (exponent > 0)
    {
        if (exponent & 1)
        {
            result = (result * base) % modulus;
        }
        base = (base * base) % modulus;
        exponent /= 2;
    }
    return result;
}
bytes::Bytes generateEcutekRacecomCanSeedKey(bytes::ByteView seed)
{
    const std::uint32_t seedWord = (static_cast<std::uint32_t>(seed[0]) << 24) |
                                   (static_cast<std::uint32_t>(seed[1]) << 16) |
                                   (static_cast<std::uint32_t>(seed[2]) << 8) | static_cast<std::uint32_t>(seed[3]);
    constexpr std::uint64_t d = 0x0A863281ULL;
    constexpr std::uint64_t n = 0x0fda9293ULL;
    const std::uint32_t decrypted = static_cast<std::uint32_t>(decryptRaceromSeed(seedWord, d, n));
    return composeBe(decrypted);
}
// encrypt_payload(), this class's OWN key table, distinct from the K-Line
// sibling's.
bytes::Bytes encryptPayloadCan(bytes::ByteView buf, std::uint32_t len)
{
    static constexpr std::uint16_t kIndex[] = {0xC85B, 0x32C0, 0xE282, 0x92A0};
    static constexpr std::uint8_t kTransform[] = {0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                  0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                  0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculatePayload(buf, len, kIndex, kTransform);
}

// ---- Kernel-upload framing: TRANSCRIBE of upload_kernel()'s padding/-------
// ---- checksum/encrypt pipeline and B6 chunking ----------------------------

struct KernelUploadPlan
{
    std::uint32_t dataLen = 0;
    std::uint32_t maxBlocks = 0;
    bytes::Bytes encryptedPayload; // length == dataLen
};

KernelUploadPlan computeKernelUploadPlan(bytes::ByteView kernelBytes)
{
    KernelUploadPlan plan;
    const std::uint32_t fileLen = static_cast<std::uint32_t>(kernelBytes.size());
    const std::uint32_t plLen = (fileLen + 3) & ~std::uint32_t(3);
    bytes::Bytes plEncr(kernelBytes.begin(), kernelBytes.end());

    plan.maxBlocks = plLen / 128;
    if (plLen % 128 != 0)
    {
        plan.maxBlocks++;
    }
    plan.dataLen = plan.maxBlocks * 128;

    plEncr.resize(plan.dataLen, 0);
    plEncr.resize(plEncr.size() - 4);

    std::uint32_t chkSum = 0;
    for (std::size_t i = 0; i < plEncr.size(); i += 4)
    {
        chkSum += (static_cast<std::uint32_t>(plEncr[i]) << 24) | (static_cast<std::uint32_t>(plEncr[i + 1]) << 16) |
                  (static_cast<std::uint32_t>(plEncr[i + 2]) << 8) | static_cast<std::uint32_t>(plEncr[i + 3]);
    }
    chkSum = 0x5aa5a55au - chkSum;

    bytes::appendU32Be(plEncr, chkSum);

    plan.encryptedPayload = encryptPayloadCan(plEncr, static_cast<std::uint32_t>(plEncr.size()));
    return plan;
}

// ---- Response fixtures ----------------------------------------------------

bytes::Bytes kernelAliveResponse()
{
    bytes::Bytes out(9, 0);
    out[4] = 0xBE;
    out[5] = 0xEF;
    out[8] = 0x41; // SUB_KERNEL_ID | 0x40
    out = composeBe(out, std::string_view{"KERN2"});
    return out; // 14 bytes
}
bytes::Bytes initConnResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x41;
    out[5] = 0x00;
    return out;
}
bytes::Bytes ecuIdResponse()
{
    bytes::Bytes out(13, 0);
    out[4] = 0xEA;
    return out;
}
bytes::Bytes vinResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x49;
    out[5] = 0x02;
    return out;
}
bytes::Bytes calIdResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x49;
    out[5] = 0x04;
    return out;
}
bytes::Bytes cvnResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x49;
    out[5] = 0x06;
    return out;
}
bytes::Bytes session03Response()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x50;
    out[5] = 0x03;
    return out;
}
bytes::Bytes session43Response()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x50;
    out[5] = 0x43;
    return out;
}
bytes::Bytes seedResponse(bytes::ByteView seed)
{
    bytes::Bytes out(10, 0);
    out[4] = 0x67;
    out[5] = 0x01;
    out[6] = seed[0];
    out[7] = seed[1];
    out[8] = seed[2];
    out[9] = seed[3];
    return out;
}
bytes::Bytes seedKeyAckResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x67;
    out[5] = 0x02;
    return out;
}
bytes::Bytes sessionSetResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x50;
    out[5] = 0x02;
    return out;
}
bytes::Bytes sid34DownloadAckResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x74;
    out[5] = 0x20;
    return out;
}
bytes::Bytes sid37StartAckResponse()
{
    bytes::Bytes out(5, 0);
    out[4] = 0x77;
    return out;
}
bytes::Bytes sid31RoutineAckResponse()
{
    bytes::Bytes out(5, 0);
    out[4] = 0x71;
    return out;
}
bytes::Bytes eepromHeaderAckResponse()
{
    bytes::Bytes out(9, 0);
    out[4] = 0xBE;
    out[5] = 0xEF;
    out[8] = 0x43; // SUB_KERNEL_READ_AREA | 0x40
    return out;
}
// 8 leading bytes (stripped, unchecked) + a 0x00..0xFF ramp.
bytes::Bytes eepromPagedataResponse264Bytes()
{
    bytes::Bytes out(8, 0xEE);
    for (int i = 0; i < 256; ++i)
    {
        out.push_back(static_cast<bytes::Byte>(i & 0xFF));
    }
    return out; // 264 bytes
}
bytes::Bytes expectedDecodedEeprom256Bytes()
{
    bytes::Bytes out;
    for (int i = 0; i < 256; ++i)
    {
        out.push_back(static_cast<bytes::Byte>(i & 0xFF));
    }
    return out;
}
// A 16-byte (already 4-byte-aligned) kernel fixture -- this class's
// upload_kernel() pads pl_encr to exactly data_len before encrypting, so
// (unlike the K-Line sibling) there is no OOB-read risk to exercise here;
// 16 bytes is used purely for parity/readability with the K-Line test.
bytes::Bytes kernelFixtureBytes()
{
    bytes::Bytes out;
    for (int i = 0; i < 16; ++i)
    {
        out.push_back(static_cast<bytes::Byte>(i));
    }
    return out;
}

// Enqueues the exact write/read sequence for one full "kernel not yet
// running" connect_bootloader() round using the Stock seed-key algorithm.
void enqueueConnectBootloaderFullInit(ScriptedCanFlashTransport& transport, bytes::ByteView seed)
{
    transport.expectWrite(requestKernelIdRequest());
    transport.queue_no_frame(); // kernel not (yet) alive

    transport.expectWrite(initConnectionRequest());
    transport.queueRead(initConnResponse());

    transport.expectWrite(ecuIdRequest());
    transport.queueRead(ecuIdResponse());

    transport.expectWrite(vinRequest());
    transport.queueRead(vinResponse());

    transport.expectWrite(calIdRequest());
    transport.queueRead(calIdResponse());

    transport.expectWrite(cvnRequest());
    transport.queueRead(cvnResponse());

    transport.expectWrite(sessionMode03Request());
    transport.queueRead(session03Response());

    transport.expectWrite(sessionMode43Request());
    transport.queueRead(session43Response());

    transport.expectWrite(seedRequestFrame());
    transport.queueRead(seedResponse(seed));

    const bytes::Bytes seedKey = generateSeedKeyStock(seed);
    transport.expectWrite(seedKeySendRequest(seedKey));
    transport.queueRead(seedKeyAckResponse());

    transport.expectWrite(sessionSetRequestBothConnected());
    transport.queueRead(sessionSetResponse());
}

// Enqueues the exact write/read sequence one upload_kernel() round produces
// for a given kernel fixture: SID34, one 0xB6 frame per block (the last one
// empty -- see sidB6TransferBlockRequest()'s call site comment below), 0x37,
// 0x31, then the post-upload single-attempt request_kernel_id() poll.
void enqueueUploadKernel(ScriptedCanFlashTransport& transport, bytes::ByteView kernelBytes,
                         std::uint32_t kernelStartAddr)
{
    const KernelUploadPlan plan = computeKernelUploadPlan(kernelBytes);
    transport.expectWrite(sid34RequestDownloadRequest(kernelStartAddr, plan.dataLen));
    transport.queueRead(sid34DownloadAckResponse());

    // lines 816-857: blockno runs 0..maxBlocks INCLUSIVE. Since dataLen ==
    // maxBlocks*128 exactly, the final (blockno==maxBlocks) iteration's chunk
    // is always empty -- an N-block kernel produces N+1 wire frames.
    for (std::uint32_t blockno = 0; blockno <= plan.maxBlocks; ++blockno)
    {
        const std::uint32_t blockAddr = kernelStartAddr + blockno * 128;
        const bytes::ByteView chunk =
            blockno < plan.maxBlocks
                ? bytes::ByteView(plan.encryptedPayload).subspan(static_cast<std::size_t>(blockno) * 128, 128)
                : bytes::ByteView{};
        transport.expectWrite(sidB6TransferBlockRequest(blockAddr, chunk));
        transport.queue_no_frame(); // response content is never inspected
    }

    transport.expectWrite(sid37StartKernelRequest());
    transport.queueRead(sid37StartAckResponse());

    transport.expectWrite(sid31StartRoutineRequest());
    transport.queueRead(sid31RoutineAckResponse());

    transport.expectWrite(requestKernelIdRequest());
    transport.queueRead(kernelAliveResponse());
}

// Enqueues the exact write/read sequence one read_mem() page (SH7055's
// single page) consumes.
void enqueueReadMem(ScriptedCanFlashTransport& transport, std::uint8_t eepromMode)
{
    transport.expectWrite(sidReadEepromRequestForSh7055(eepromMode));
    transport.queueRead(eepromHeaderAckResponse());
    transport.queueRead(eepromPagedataResponse264Bytes());
}

Result<FlashPlan> makeCanPlan(DensoSecurityVariant security, EepromReadMode mode, bytes::Bytes kernelBytes,
                              std::uint32_t kernelAddr)
{
    return build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput{
        .operation = FlashOperation::Read,
        .family = FlashFamily::DensoSh705xEepromCan,
        .target_id = "sub_ecu_eeprom_denso_sh7055_densocan",
        .mcu_name = "SH7055",
        .flash_method = "sub_ecu_eeprom_denso_sh7055_densocan",
        .kernel = KernelImage{.id = "k", .load_address = kernelAddr, .bytes = std::move(kernelBytes)},
        .mode = mode,
        .security = security,
        .eeprom_region = MemoryRegion{.start = 0, .length = 0x100},
    });
}

Result<FlashPlan> valid_can_plan(EepromReadMode mode = EepromReadMode::Mode2)
{
    return makeCanPlan(DensoSecurityVariant::Stock, mode, kernelFixtureBytes(), kKernelStartAddr);
}

static_assert(kRequestId == 0x7e0, "kernel-id/handshake frame literals above assume request_id == 0x7e0");

} // namespace

TEST(DensoSh705xEepromCanExecutorTest, TransportSetupReturnsThePlansWireParameters)
{
    auto plan = valid_can_plan();
    ASSERT_TRUE(plan.has_value());

    DensoSh705xEepromCanExecutor executor;
    const auto setup = executor.transport_setup(*plan);

    ASSERT_TRUE(setup.has_value()) << setup.error().detail;
    EXPECT_EQ(setup->bitrate, 500000);
    EXPECT_EQ(setup->request_id, 0x7e0u);
    EXPECT_EQ(setup->response_id, 0x7e8u);
    EXPECT_FALSE(setup->extended_id);
}

TEST(DensoSh705xEepromCanExecutorTest, WrongFamilyPlanIsRejectedWithNoTransportCalls)
{
    auto plan = build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput{
        .operation = FlashOperation::Read,
        .family = FlashFamily::DensoSh705xEepromKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .flash_method = "sub_ecu_eeprom_denso_sh7055_kline",
        .kernel = KernelImage{.id = "k", .load_address = kKernelStartAddr, .bytes = {0x01, 0x02, 0x03, 0x04}},
        .mode = EepromReadMode::Mode2,
        .security = DensoSecurityVariant::Stock,
        .eeprom_region = MemoryRegion{.start = 0, .length = 0x100},
    });
    ASSERT_TRUE(plan.has_value());

    DensoSh705xEepromCanExecutor executor;
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(transport.scriptConsumed()); // nothing was ever queued or consumed
}

TEST(DensoSh705xEepromCanExecutorTest, KernelAlreadyRunningSkipsBootloaderMatchesLegacyTrace)
{
    auto plan = valid_can_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    transport.expectWrite(requestKernelIdRequest());
    transport.queueRead(kernelAliveResponse());
    enqueueReadMem(transport, 2);

    DensoSh705xEepromCanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << to_string(result.error().kind) << ": " << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Read);
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, expectedDecodedEeprom256Bytes());
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(DensoSh705xEepromCanExecutorTest, FullBootloaderStockSecurityMode2MatchesLegacyTrace)
{
    auto plan = valid_can_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    enqueueConnectBootloaderFullInit(transport, seed);
    enqueueUploadKernel(transport, kernelFixtureBytes(), kKernelStartAddr);
    enqueueReadMem(transport, 2);

    DensoSh705xEepromCanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << to_string(result.error().kind) << ": " << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Read);
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, expectedDecodedEeprom256Bytes());
    EXPECT_TRUE(transport.scriptConsumed());
}

// Pins that connect_bootloader()'s security-variant dispatch really does
// route to a distinct generate_*_seed_key() function per DensoSecurityVariant
// value, by driving the executor far enough to capture each variant's
// "0x27,0x02,<key>" frame from an IDENTICAL seed and proving all four differ.
// Each run is deliberately stopped right after the seed-key-send write (its
// own response read is scripted as "no frame", tripping ErrorKind::Timeout
// immediately after) -- keeping this test fast without needing a full kernel
// upload + EEPROM read per variant, mirroring the deleted characterization
// test's own "stopped short" shape.
TEST(DensoSh705xEepromCanExecutorTest, AllFourSecurityVariantsProduceDistinctSeedKeyFrames)
{
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    const DensoSecurityVariant variants[] = {DensoSecurityVariant::Stock, DensoSecurityVariant::EcuTek,
                                             DensoSecurityVariant::Cobb, DensoSecurityVariant::EcuTekRaceRom};
    std::vector<bytes::Bytes> seedKeyFrames;

    for (DensoSecurityVariant security : variants)
    {
        auto plan = makeCanPlan(security, EepromReadMode::Mode2, kernelFixtureBytes(), kKernelStartAddr);
        ASSERT_TRUE(plan.has_value());

        ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
        transport.expectWrite(requestKernelIdRequest());
        transport.queue_no_frame();
        transport.expectWrite(initConnectionRequest());
        transport.queueRead(initConnResponse());
        transport.expectWrite(ecuIdRequest());
        transport.queueRead(ecuIdResponse());
        transport.expectWrite(vinRequest());
        transport.queueRead(vinResponse());
        transport.expectWrite(calIdRequest());
        transport.queueRead(calIdResponse());
        transport.expectWrite(cvnRequest());
        transport.queueRead(cvnResponse());
        transport.expectWrite(sessionMode03Request());
        transport.queueRead(session03Response());
        transport.expectWrite(sessionMode43Request());
        transport.queueRead(session43Response());
        transport.expectWrite(seedRequestFrame());
        transport.queueRead(seedResponse(seed));
        // The seed-key-send frame itself is captured via expectWrite() below
        // (whichever bytes the executor sends must match, or the write fails
        // with ErrorKind::Internal) -- we don't know its expected content
        // ahead of time here (that's exactly what's under test), so instead
        // we let it through as a wildcard by pre-registering an expectation
        // per candidate key below.
        const bytes::Bytes expectedKey = [&]() -> bytes::Bytes
        {
            switch (security)
            {
            case DensoSecurityVariant::Stock:
                return generateSeedKeyStock(seed);
            case DensoSecurityVariant::EcuTek:
                return generateEcutekSeedKeyPlain(seed);
            case DensoSecurityVariant::Cobb:
                return generateCobbSeedKey(seed);
            case DensoSecurityVariant::EcuTekRaceRom:
                return generateEcutekRacecomCanSeedKey(seed);
            }
            return {};
        }();
        transport.expectWrite(seedKeySendRequest(expectedKey));
        transport.queue_no_frame(); // no response at all -> Timeout, stopping the round here

        DensoSh705xEepromCanExecutor executor;
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
        EXPECT_EQ(transport.writesConsumed(), 10u); // kernel-id probe + 9 handshake writes
        EXPECT_TRUE(transport.scriptConsumed());

        seedKeyFrames.push_back(seedKeySendRequest(expectedKey));
    }

    for (std::size_t i = 0; i < seedKeyFrames.size(); ++i)
    {
        for (std::size_t j = i + 1; j < seedKeyFrames.size(); ++j)
        {
            EXPECT_NE(seedKeyFrames[i], seedKeyFrames[j])
                << "variants " << i << " and " << j << " produced the same seed-key frame";
        }
    }
}

TEST(DensoSh705xEepromCanExecutorTest, NoResponseAtSeedRequestReturnsTimeout)
{
    auto plan = valid_can_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    transport.expectWrite(requestKernelIdRequest());
    transport.queue_no_frame();
    transport.expectWrite(initConnectionRequest());
    transport.queueRead(initConnResponse());
    transport.expectWrite(ecuIdRequest());
    transport.queueRead(ecuIdResponse());
    transport.expectWrite(vinRequest());
    transport.queueRead(vinResponse());
    transport.expectWrite(calIdRequest());
    transport.queueRead(calIdResponse());
    transport.expectWrite(cvnRequest());
    transport.queueRead(cvnResponse());
    transport.expectWrite(sessionMode03Request());
    transport.queueRead(session03Response());
    transport.expectWrite(sessionMode43Request());
    transport.queueRead(session43Response());
    transport.expectWrite(seedRequestFrame());
    transport.queue_no_frame(); // no response at all -> Timeout

    DensoSh705xEepromCanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
}

TEST(DensoSh705xEepromCanExecutorTest, MalformedSeedResponseReturnsBadResponse)
{
    auto plan = valid_can_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    transport.expectWrite(requestKernelIdRequest());
    transport.queue_no_frame();
    transport.expectWrite(initConnectionRequest());
    transport.queueRead(initConnResponse());
    transport.expectWrite(ecuIdRequest());
    transport.queueRead(ecuIdResponse());
    transport.expectWrite(vinRequest());
    transport.queueRead(vinResponse());
    transport.expectWrite(calIdRequest());
    transport.queueRead(calIdResponse());
    transport.expectWrite(cvnRequest());
    transport.queueRead(cvnResponse());
    transport.expectWrite(sessionMode03Request());
    transport.queueRead(session03Response());
    transport.expectWrite(sessionMode43Request());
    transport.queueRead(session43Response());
    transport.expectWrite(seedRequestFrame());
    bytes::Bytes malformed(6, 0);
    malformed[4] = 0x00; // should be 0x67
    malformed[5] = 0x00; // should be 0x01
    transport.queueRead(malformed);

    DensoSh705xEepromCanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
}

TEST(DensoSh705xEepromCanExecutorTest, CancellationDuringKernelUploadReturnsCancelled)
{
    auto plan = valid_can_plan(EepromReadMode::Mode2);
    ASSERT_TRUE(plan.has_value());

    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::Open};
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    enqueueConnectBootloaderFullInit(transport, seed);
    enqueueUploadKernel(transport, kernelFixtureBytes(), kKernelStartAddr);
    enqueueReadMem(transport, 2);

    DensoSh705xEepromCanExecutor executor;
    FakeClock clock;
    // Trips partway through upload_kernel()'s chunk loop: connect_bootloader()
    // completes fully (kernel not alive -> probe + full init/ecuid/vin/calid/
    // cvn/session03/session43/seedreq/seedkeysend/sessionset = 10 writes),
    // then upload_kernel() writes its SID34 download request (write #11)
    // before the per-chunk cancellation guard trips -- the first 0xB6 chunk
    // (what would be write #12) is never written. N tuned empirically against
    // this exact trace's total cancellation.cancelled() call count.
    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(75);
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.writesConsumed(), 11u);
}

} // namespace fastecu::flash
