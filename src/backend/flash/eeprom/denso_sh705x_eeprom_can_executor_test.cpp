#include "src/backend/ports/testing/result_matchers.h"
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
using bytes::ComposeBe;
using bytes::U24;
using namespace bytes::literals;

// eeprom_ecu_subaru_denso_sh705x_can_operation.cpp's serial->set_can_source_
// address(0x7e0)/set_iso15765_destination_address(0x7e8) (lines 61-64),
// mirrored by the builder's DensoSh705xEepromCanPlan.
constexpr std::uint32_t kRequestId = 0x7e0;

// Matches the built-in catalog's CAN protocol entries'
// kernel_addr for McuType "SH7055" (kKernelBlocksSH7055[0].start), the same
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

bytes::Bytes InitConnectionRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x01, 0x00};
}
bytes::Bytes EcuIdRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0xAA};
}
bytes::Bytes VinRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x09, 0x02};
}
bytes::Bytes CalIdRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x09, 0x04};
}
bytes::Bytes CvnRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x09, 0x06};
}
bytes::Bytes SessionMode03Request()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x10, 0x03};
}
bytes::Bytes SessionMode43Request()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x10, 0x43};
}
bytes::Bytes SeedRequestFrame()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x27, 0x01};
}
bytes::Bytes SeedKeySendRequest(bytes::ByteView key)
{
    return ComposeBe(bytes::Bytes{0x00, 0x00, 0x07, 0xE0}, 0x27_b, 0x02_b, key);
}
// Every fixture below gives positive responses to both prior session-mode
// requests, so both flags are true and both bytes are appended.
bytes::Bytes SessionSetRequestBothConnected()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x10, 0x02, 0x42};
}
// request_kernel_id(), lines 1355-1390: UNLIKE the K-Line sibling's
// request_kernel_id(), NOT checksum-terminated.
bytes::Bytes RequestKernelIdRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
}
bytes::Bytes Sid34RequestDownloadRequest(std::uint32_t start_address, std::uint32_t data_len)
{
    return ComposeBe(bytes::Bytes{0x00, 0x00, 0x07, 0xE0}, 0x34_b, 0x04_b, 0x33_b, U24(start_address), U24(data_len));
}
bytes::Bytes SidB6TransferBlockRequest(std::uint32_t block_addr, bytes::ByteView payload)
{
    return ComposeBe(bytes::Bytes{0x00, 0x00, 0x07, 0xE0}, 0xB6_b, U24(block_addr), payload);
}
bytes::Bytes Sid37StartKernelRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0x37};
}
bytes::Bytes Sid31StartRoutineRequest()
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
    EXPECT_THAT(SeedKeySendRequest(bytes::Bytes{0x33, 0x44}),
                ElementsAre(0x00, 0x00, 0x07, 0xE0, 0x27, 0x02, 0x33, 0x44));
}

// sid34RequestDownloadRequest(0x002000, 0x000040):
// payload = [0x34, 0x04, 0x33, u24(0x002000), u24(0x000040)]
//         = [0x34, 0x04, 0x33, 0x00, 0x20, 0x00, 0x00, 0x00, 0x40].
TEST(DensoSh705xEepromCanExecutorTest, Sid34RequestDownloadRequestMatchesHardcodedWireBytes)
{
    EXPECT_THAT(Sid34RequestDownloadRequest(0x002000, 0x000040),
                ElementsAre(0x00, 0x00, 0x07, 0xE0, 0x34, 0x04, 0x33, 0x00, 0x20, 0x00, 0x00, 0x00, 0x40));
}

// sidB6TransferBlockRequest(0x003000, {0x55, 0x66, 0x77}):
// payload = [0xB6, u24(0x003000), 0x55, 0x66, 0x77]
//         = [0xB6, 0x00, 0x30, 0x00, 0x55, 0x66, 0x77].
TEST(DensoSh705xEepromCanExecutorTest, SidB6TransferBlockRequestMatchesHardcodedWireBytes)
{
    EXPECT_THAT(SidB6TransferBlockRequest(0x003000, bytes::Bytes{0x55, 0x66, 0x77}),
                ElementsAre(0x00, 0x00, 0x07, 0xE0, 0xB6, 0x00, 0x30, 0x00, 0x55, 0x66, 0x77));
}
// read_mem(), for McuType "SH7055" (kEepromBlocksSH7055[0] == {start=0,
// len=0x100}): reduces to a single request with addr=0, pagesize=0x100.
bytes::Bytes SidReadEepromRequestForSh7055(std::uint8_t eeprom_mode)
{
    return {
        0x00,        0x00, 0x07, 0xe0, 0xBE, 0xEF, 0x00, 0x07,
        0x07, // SUB_KERNEL_READ_EEPROM
        eeprom_mode,
        0x00, // addr>>16 (addr == 0)
        0x00, // addr>>8
        0x00, // addr
        0x01, // pagesize>>8 (pagesize == 0x100)
        0x00, // pagesize
    };
}

// ---- Seed-key algorithms: TRANSCRIBE of each generate_*_seed_key() --------

bytes::Bytes GenerateSeedKeyStock(bytes::ByteView seed)
{
    static constexpr auto kIndex =
        std::to_array<std::uint16_t>({0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71, 0xB046, 0x7F4A,
                                      0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B});
    static constexpr auto kTransform =
        std::to_array<std::uint8_t>({0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
                                     0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8});
    return ssm_protocol::CalculateSeedKey(seed, kIndex, kTransform);
}
bytes::Bytes GenerateEcutekSeedKeyPlain(bytes::ByteView seed)
{
    static constexpr auto kIndex =
        std::to_array<std::uint16_t>({0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71, 0xB046, 0x7F4A,
                                      0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B});
    static constexpr auto kTransform =
        std::to_array<std::uint8_t>({0x4, 0x2, 0x5, 0x1, 0x8, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
                                     0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8});
    return ssm_protocol::CalculateSeedKey(seed, kIndex, kTransform);
}
bytes::Bytes GenerateCobbSeedKey(bytes::ByteView seed)
{
    static constexpr auto kIndex =
        std::to_array<std::uint16_t>({0x9DDB, 0x9CFB, 0x9B9A, 0x6136, 0x59E1, 0xBA03, 0xD683, 0x7092, 0x9E05, 0x8723,
                                      0xF998, 0x15BB, 0xB8D5, 0xFF0C, 0x9D91, 0x24B9});
    static constexpr auto kTransform =
        std::to_array<std::uint8_t>({0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
                                     0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8});
    return ssm_protocol::CalculateSeedKey(seed, kIndex, kTransform);
}
std::uint64_t DecryptRaceromSeed(std::uint64_t base, std::uint64_t exponent, std::uint64_t modulus)
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
bytes::Bytes GenerateEcutekRacecomCanSeedKey(bytes::ByteView seed)
{
    const std::uint32_t seed_word = (static_cast<std::uint32_t>(seed[0]) << 24) |
                                    (static_cast<std::uint32_t>(seed[1]) << 16) |
                                    (static_cast<std::uint32_t>(seed[2]) << 8) | static_cast<std::uint32_t>(seed[3]);
    constexpr std::uint64_t kD = 0x0A863281ULL;
    constexpr std::uint64_t kN = 0x0fda9293ULL;
    const std::uint32_t decrypted = static_cast<std::uint32_t>(DecryptRaceromSeed(seed_word, kD, kN));
    return ComposeBe(decrypted);
}
// encrypt_payload(), this class's OWN key table, distinct from the K-Line
// sibling's.
bytes::Bytes EncryptPayloadCan(bytes::ByteView buf, std::uint32_t len)
{
    static constexpr auto kIndex = std::to_array<std::uint16_t>({0xC85B, 0x32C0, 0xE282, 0x92A0});
    static constexpr auto kTransform =
        std::to_array<std::uint8_t>({0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
                                     0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8});
    return ssm_protocol::CalculatePayload(buf, len, kIndex, kTransform);
}

// ---- Kernel-upload framing: TRANSCRIBE of upload_kernel()'s padding/-------
// ---- checksum/encrypt pipeline and B6 chunking ----------------------------

struct KernelUploadPlan
{
    std::uint32_t data_len = 0;
    std::uint32_t max_blocks = 0;
    bytes::Bytes encrypted_payload; // length == dataLen
};

KernelUploadPlan ComputeKernelUploadPlan(bytes::ByteView kernel_bytes)
{
    KernelUploadPlan plan;
    const std::uint32_t file_len = static_cast<std::uint32_t>(kernel_bytes.size());
    const std::uint32_t pl_len = (file_len + 3) & ~std::uint32_t(3);
    bytes::Bytes pl_encr(kernel_bytes.begin(), kernel_bytes.end());

    plan.max_blocks = pl_len / 128;
    if (pl_len % 128 != 0)
    {
        plan.max_blocks++;
    }
    plan.data_len = plan.max_blocks * 128;

    pl_encr.resize(plan.data_len, 0);
    pl_encr.resize(pl_encr.size() - 4);

    std::uint32_t chk_sum = 0;
    for (std::size_t i = 0; i < pl_encr.size(); i += 4)
    {
        chk_sum += (static_cast<std::uint32_t>(pl_encr[i]) << 24) | (static_cast<std::uint32_t>(pl_encr[i + 1]) << 16) |
                   (static_cast<std::uint32_t>(pl_encr[i + 2]) << 8) | static_cast<std::uint32_t>(pl_encr[i + 3]);
    }
    chk_sum = 0x5aa5a55aU - chk_sum;

    bytes::AppendU32Be(pl_encr, chk_sum);

    plan.encrypted_payload = EncryptPayloadCan(pl_encr, static_cast<std::uint32_t>(pl_encr.size()));
    return plan;
}

// ---- Response fixtures ----------------------------------------------------

bytes::Bytes KernelAliveResponse()
{
    bytes::Bytes out(9, 0);
    out[4] = 0xBE;
    out[5] = 0xEF;
    out[8] = 0x41; // SUB_KERNEL_ID | 0x40
    out = ComposeBe(out, std::string_view{"KERN2"});
    return out; // 14 bytes
}
bytes::Bytes InitConnResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x41;
    out[5] = 0x00;
    return out;
}
bytes::Bytes EcuIdResponse()
{
    bytes::Bytes out(13, 0);
    out[4] = 0xEA;
    return out;
}
bytes::Bytes VinResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x49;
    out[5] = 0x02;
    return out;
}
bytes::Bytes CalIdResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x49;
    out[5] = 0x04;
    return out;
}
bytes::Bytes CvnResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x49;
    out[5] = 0x06;
    return out;
}
bytes::Bytes Session03Response()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x50;
    out[5] = 0x03;
    return out;
}
bytes::Bytes Session43Response()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x50;
    out[5] = 0x43;
    return out;
}
bytes::Bytes SeedResponse(bytes::ByteView seed)
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
bytes::Bytes SeedKeyAckResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x67;
    out[5] = 0x02;
    return out;
}
bytes::Bytes SessionSetResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x50;
    out[5] = 0x02;
    return out;
}
bytes::Bytes Sid34DownloadAckResponse()
{
    bytes::Bytes out(6, 0);
    out[4] = 0x74;
    out[5] = 0x20;
    return out;
}
bytes::Bytes Sid37StartAckResponse()
{
    bytes::Bytes out(5, 0);
    out[4] = 0x77;
    return out;
}
bytes::Bytes Sid31RoutineAckResponse()
{
    bytes::Bytes out(5, 0);
    out[4] = 0x71;
    return out;
}
bytes::Bytes EepromHeaderAckResponse()
{
    bytes::Bytes out(9, 0);
    out[4] = 0xBE;
    out[5] = 0xEF;
    out[8] = 0x43; // SUB_KERNEL_READ_AREA | 0x40
    return out;
}
// 8 leading bytes (stripped, unchecked) + a 0x00..0xFF ramp.
bytes::Bytes EepromPagedataResponse264Bytes()
{
    bytes::Bytes out(8, 0xEE);
    for (int i = 0; i < 256; ++i)
    {
        out.push_back(static_cast<bytes::Byte>(i));
    }
    return out; // 264 bytes
}
bytes::Bytes ExpectedDecodedEeprom256Bytes()
{
    bytes::Bytes out;
    for (int i = 0; i < 256; ++i)
    {
        out.push_back(static_cast<bytes::Byte>(i));
    }
    return out;
}
// A 16-byte (already 4-byte-aligned) kernel fixture -- this class's
// upload_kernel() pads pl_encr to exactly data_len before encrypting, so
// (unlike the K-Line sibling) there is no OOB-read risk to exercise here;
// 16 bytes is used purely for parity/readability with the K-Line test.
bytes::Bytes KernelFixtureBytes()
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
void EnqueueConnectBootloaderFullInit(ScriptedCanFlashTransport& transport, bytes::ByteView seed)
{
    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueNoFrame(); // kernel not (yet) alive

    transport.ExpectWrite(InitConnectionRequest());
    transport.QueueRead(InitConnResponse());

    transport.ExpectWrite(EcuIdRequest());
    transport.QueueRead(EcuIdResponse());

    transport.ExpectWrite(VinRequest());
    transport.QueueRead(VinResponse());

    transport.ExpectWrite(CalIdRequest());
    transport.QueueRead(CalIdResponse());

    transport.ExpectWrite(CvnRequest());
    transport.QueueRead(CvnResponse());

    transport.ExpectWrite(SessionMode03Request());
    transport.QueueRead(Session03Response());

    transport.ExpectWrite(SessionMode43Request());
    transport.QueueRead(Session43Response());

    transport.ExpectWrite(SeedRequestFrame());
    transport.QueueRead(SeedResponse(seed));

    const bytes::Bytes seed_key = GenerateSeedKeyStock(seed);
    transport.ExpectWrite(SeedKeySendRequest(seed_key));
    transport.QueueRead(SeedKeyAckResponse());

    transport.ExpectWrite(SessionSetRequestBothConnected());
    transport.QueueRead(SessionSetResponse());
}

// Enqueues the exact write/read sequence one upload_kernel() round produces
// for a given kernel fixture: SID34, one 0xB6 frame per block (the last one
// empty -- see sidB6TransferBlockRequest()'s call site comment below), 0x37,
// 0x31, then the post-upload single-attempt request_kernel_id() poll.
void EnqueueUploadKernel(ScriptedCanFlashTransport& transport, bytes::ByteView kernel_bytes,
                         std::uint32_t kernel_start_addr)
{
    const KernelUploadPlan plan = ComputeKernelUploadPlan(kernel_bytes);
    transport.ExpectWrite(Sid34RequestDownloadRequest(kernel_start_addr, plan.data_len));
    transport.QueueRead(Sid34DownloadAckResponse());

    // lines 816-857: blockno runs 0..maxBlocks INCLUSIVE. Since dataLen ==
    // maxBlocks*128 exactly, the final (blockno==maxBlocks) iteration's chunk
    // is always empty -- an N-block kernel produces N+1 wire frames.
    for (std::uint32_t blockno = 0; blockno <= plan.max_blocks; ++blockno)
    {
        const std::uint32_t block_addr = kernel_start_addr + blockno * 128;
        const bytes::ByteView chunk =
            blockno < plan.max_blocks
                ? bytes::ByteView(plan.encrypted_payload).subspan(static_cast<std::size_t>(blockno) * 128, 128)
                : bytes::ByteView{};
        transport.ExpectWrite(SidB6TransferBlockRequest(block_addr, chunk));
        transport.QueueNoFrame(); // response content is never inspected
    }

    transport.ExpectWrite(Sid37StartKernelRequest());
    transport.QueueRead(Sid37StartAckResponse());

    transport.ExpectWrite(Sid31StartRoutineRequest());
    transport.QueueRead(Sid31RoutineAckResponse());

    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueRead(KernelAliveResponse());
}

// Enqueues the exact write/read sequence one read_mem() page (SH7055's
// single page) consumes.
void EnqueueReadMem(ScriptedCanFlashTransport& transport, std::uint8_t eeprom_mode)
{
    transport.ExpectWrite(SidReadEepromRequestForSh7055(eeprom_mode));
    transport.QueueRead(EepromHeaderAckResponse());
    transport.QueueRead(EepromPagedataResponse264Bytes());
}

Result<FlashPlan> MakeCanPlan(DensoSecurityVariant security, EepromReadMode mode, bytes::Bytes kernel_bytes,
                              std::uint32_t kernel_addr)
{
    return BuildDensoSh705xEepromPlan(DensoSh705xEepromInput{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kDensoSh705xEepromCan,
        .target_id = "sub_ecu_eeprom_denso_sh7055_densocan",
        .mcu_name = "SH7055",
        .flash_method = "sub_ecu_eeprom_denso_sh7055_densocan",
        .kernel = KernelImage{.id = "k", .load_address = kernel_addr, .bytes = std::move(kernel_bytes)},
        .mode = mode,
        .security = security,
        .eeprom_region = MemoryRegion{.start = 0, .length = 0x100},
    });
}

Result<FlashPlan> ValidCanPlan(EepromReadMode mode = EepromReadMode::kMode2)
{
    return MakeCanPlan(DensoSecurityVariant::kStock, mode, KernelFixtureBytes(), kKernelStartAddr);
}

static_assert(kRequestId == 0x7e0, "kernel-id/handshake frame literals above assume request_id == 0x7e0");

} // namespace

TEST(DensoSh705xEepromCanExecutorTest, TransportSetupReturnsThePlansWireParameters)
{
    auto plan = ValidCanPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    DensoSh705xEepromCanExecutor executor;
    const auto setup = executor.TransportSetup(*plan);

    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->bitrate, 500000);
    EXPECT_EQ(setup->request_id, 0x7e0U);
    EXPECT_EQ(setup->response_id, 0x7e8U);
    EXPECT_FALSE(setup->extended_id);
}

TEST(DensoSh705xEepromCanExecutorTest, WrongFamilyPlanIsRejectedWithNoTransportCalls)
{
    auto plan = BuildDensoSh705xEepromPlan(DensoSh705xEepromInput{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kDensoSh705xEepromKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .flash_method = "sub_ecu_eeprom_denso_sh7055_kline",
        .kernel = KernelImage{.id = "k", .load_address = kKernelStartAddr, .bytes = {0x01, 0x02, 0x03, 0x04}},
        .mode = EepromReadMode::kMode2,
        .security = DensoSecurityVariant::kStock,
        .eeprom_region = MemoryRegion{.start = 0, .length = 0x100},
    });
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    DensoSh705xEepromCanExecutor executor;
    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_TRUE(transport.ScriptConsumed()); // nothing was ever queued or consumed
}

TEST(DensoSh705xEepromCanExecutorTest, KernelAlreadyRunningSkipsBootloaderMatchesLegacyTrace)
{
    auto plan = ValidCanPlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueRead(KernelAliveResponse());
    EnqueueReadMem(transport, 2);

    DensoSh705xEepromCanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kRead);
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, ExpectedDecodedEeprom256Bytes());
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(DensoSh705xEepromCanExecutorTest, FullBootloaderStockSecurityMode2MatchesLegacyTrace)
{
    auto plan = ValidCanPlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    EnqueueConnectBootloaderFullInit(transport, seed);
    EnqueueUploadKernel(transport, KernelFixtureBytes(), kKernelStartAddr);
    EnqueueReadMem(transport, 2);

    DensoSh705xEepromCanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kRead);
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, ExpectedDecodedEeprom256Bytes());
    EXPECT_TRUE(transport.ScriptConsumed());
}

// Pins that connect_bootloader()'s security-variant dispatch really does
// route to a distinct generate_*_seed_key() function per DensoSecurityVariant
// value, by driving the executor far enough to capture each variant's
// "0x27,0x02,<key>" frame from an IDENTICAL seed and proving all four differ.
// Each run is deliberately stopped right after the seed-key-send write (its
// own response read is scripted as "no frame", tripping ErrorKind::kTimeout
// immediately after) -- keeping this test fast without needing a full kernel
// upload + EEPROM read per variant, mirroring the deleted characterization
// test's own "stopped short" shape.
TEST(DensoSh705xEepromCanExecutorTest, AllFourSecurityVariantsProduceDistinctSeedKeyFrames)
{
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    static constexpr auto kVariants =
        std::to_array<DensoSecurityVariant>({DensoSecurityVariant::kStock, DensoSecurityVariant::kEcuTek,
                                             DensoSecurityVariant::kCobb, DensoSecurityVariant::kEcuTekRaceRom});
    std::vector<bytes::Bytes> seed_key_frames;

    for (DensoSecurityVariant security : kVariants)
    {
        auto plan = MakeCanPlan(security, EepromReadMode::kMode2, KernelFixtureBytes(), kKernelStartAddr);
        ASSERT_THAT(plan, fastecu::testing::IsOk());

        ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
        transport.ExpectWrite(RequestKernelIdRequest());
        transport.QueueNoFrame();
        transport.ExpectWrite(InitConnectionRequest());
        transport.QueueRead(InitConnResponse());
        transport.ExpectWrite(EcuIdRequest());
        transport.QueueRead(EcuIdResponse());
        transport.ExpectWrite(VinRequest());
        transport.QueueRead(VinResponse());
        transport.ExpectWrite(CalIdRequest());
        transport.QueueRead(CalIdResponse());
        transport.ExpectWrite(CvnRequest());
        transport.QueueRead(CvnResponse());
        transport.ExpectWrite(SessionMode03Request());
        transport.QueueRead(Session03Response());
        transport.ExpectWrite(SessionMode43Request());
        transport.QueueRead(Session43Response());
        transport.ExpectWrite(SeedRequestFrame());
        transport.QueueRead(SeedResponse(seed));
        // The seed-key-send frame itself is captured via expectWrite() below
        // (whichever bytes the executor sends must match, or the write fails
        // with ErrorKind::kInternal) -- we don't know its expected content
        // ahead of time here (that's exactly what's under test), so instead
        // we let it through as a wildcard by pre-registering an expectation
        // per candidate key below.
        const bytes::Bytes expected_key = [&]() -> bytes::Bytes
        {
            switch (security)
            {
            case DensoSecurityVariant::kStock:
                return GenerateSeedKeyStock(seed);
            case DensoSecurityVariant::kEcuTek:
                return GenerateEcutekSeedKeyPlain(seed);
            case DensoSecurityVariant::kCobb:
                return GenerateCobbSeedKey(seed);
            case DensoSecurityVariant::kEcuTekRaceRom:
                return GenerateEcutekRacecomCanSeedKey(seed);
            }
            return {};
        }();
        transport.ExpectWrite(SeedKeySendRequest(expected_key));
        transport.QueueNoFrame(); // no response at all -> Timeout, stopping the round here

        DensoSh705xEepromCanExecutor executor;
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;

        ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErr(ErrorKind::kTimeout));
        EXPECT_EQ(transport.WritesConsumed(), 10U); // kernel-id probe + 9 handshake writes
        EXPECT_TRUE(transport.ScriptConsumed());

        seed_key_frames.push_back(SeedKeySendRequest(expected_key));
    }

    for (std::size_t i = 0; i < seed_key_frames.size(); ++i)
    {
        for (std::size_t j = i + 1; j < seed_key_frames.size(); ++j)
        {
            EXPECT_NE(seed_key_frames[i], seed_key_frames[j])
                << "variants " << i << " and " << j << " produced the same seed-key frame";
        }
    }
}

TEST(DensoSh705xEepromCanExecutorTest, NoResponseAtSeedRequestReturnsTimeout)
{
    auto plan = ValidCanPlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueNoFrame();
    transport.ExpectWrite(InitConnectionRequest());
    transport.QueueRead(InitConnResponse());
    transport.ExpectWrite(EcuIdRequest());
    transport.QueueRead(EcuIdResponse());
    transport.ExpectWrite(VinRequest());
    transport.QueueRead(VinResponse());
    transport.ExpectWrite(CalIdRequest());
    transport.QueueRead(CalIdResponse());
    transport.ExpectWrite(CvnRequest());
    transport.QueueRead(CvnResponse());
    transport.ExpectWrite(SessionMode03Request());
    transport.QueueRead(Session03Response());
    transport.ExpectWrite(SessionMode43Request());
    transport.QueueRead(Session43Response());
    transport.ExpectWrite(SeedRequestFrame());
    transport.QueueNoFrame(); // no response at all -> Timeout

    DensoSh705xEepromCanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kTimeout));
}

TEST(DensoSh705xEepromCanExecutorTest, MalformedSeedResponseReturnsBadResponse)
{
    auto plan = ValidCanPlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(RequestKernelIdRequest());
    transport.QueueNoFrame();
    transport.ExpectWrite(InitConnectionRequest());
    transport.QueueRead(InitConnResponse());
    transport.ExpectWrite(EcuIdRequest());
    transport.QueueRead(EcuIdResponse());
    transport.ExpectWrite(VinRequest());
    transport.QueueRead(VinResponse());
    transport.ExpectWrite(CalIdRequest());
    transport.QueueRead(CalIdResponse());
    transport.ExpectWrite(CvnRequest());
    transport.QueueRead(CvnResponse());
    transport.ExpectWrite(SessionMode03Request());
    transport.QueueRead(Session03Response());
    transport.ExpectWrite(SessionMode43Request());
    transport.QueueRead(Session43Response());
    transport.ExpectWrite(SeedRequestFrame());
    bytes::Bytes malformed(6, 0);
    malformed[4] = 0x00; // should be 0x67
    malformed[5] = 0x00; // should be 0x01
    transport.QueueRead(malformed);

    DensoSh705xEepromCanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
}

TEST(DensoSh705xEepromCanExecutorTest, CancellationDuringKernelUploadReturnsCancelled)
{
    auto plan = ValidCanPlan(EepromReadMode::kMode2);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedCanFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    EnqueueConnectBootloaderFullInit(transport, seed);
    EnqueueUploadKernel(transport, KernelFixtureBytes(), kKernelStartAddr);
    EnqueueReadMem(transport, 2);

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
    cancellation.CancelOnCheck(75);
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 11U);
}

} // namespace fastecu::flash
