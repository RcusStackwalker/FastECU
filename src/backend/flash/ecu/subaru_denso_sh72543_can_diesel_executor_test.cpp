// Equivalence tests for SubaruDensoSh72543CanDieselExecutor, the portable
// replacement for FlashEcuSubaruDensoSH72543CanDieselOperation's
// connect_bootloader(), read_memory(), write_memory(), reflash_block() and
// erase_memory(). Every scripted exchange cites the line of
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh72543_can_diesel_operation.cpp
// it was transcribed from.
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::flash::build_subaru_denso_sh72543_can_diesel_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruDensoSh72543CanDieselExecutor;
using fastecu::flash::SubaruDensoSh72543CanDieselPlan;
using testing::Contains;
using testing::Each;
using testing::IsEmpty;
using testing::Pair;

// Records every ctx.clock.sleep() argument so the executor's inter-exchange
// settles can be asserted as a sequence. Same shape as the wave-2
// (subaru_denso_sh7055_02_executor_test.cpp) and wave-3
// (subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp) recording clocks: a
// FakeClock with one extra hook, so no fake or port changes shape.
class RecordingClock final : public FakeClock
{
  public:
    fastecu::Status sleep(int ms, const fastecu::ICancellationToken& cancellation) override
    {
        sleep_calls.push_back(ms);
        return FakeClock::sleep(ms, cancellation);
    }

    std::vector<int> sleep_calls;
};

constexpr std::string_view kProtocol = "sub_ecu_denso_sh72543_can_diesel";
constexpr std::string_view kMcu = "SH72543d";

// fblocks_SH72543d is the wave's only single-block table: [0] = {0x00008000,
// 0x1F7F00}, with the 0x0-0x8000 entry commented out. The image is still based
// at address 0 -- read_memory prepends 0x8000 of 0xFF, and write_memory offsets
// its buffer pointer by fblocks[0].start before reflash_block indexes it
// block-relative (lines 1129 and 1217).
constexpr std::uint32_t kImageStart = 0x00000000;
constexpr std::uint32_t kBlockStart = 0x00008000;
constexpr std::uint32_t kBlockLength = 0x1F7F00;
constexpr std::size_t kImageSize = 0x200000;
constexpr std::uint32_t kPageSize = 0x100;

// Writes scripted by scriptBenchConnect and scriptEraseMemory. Named so
// WriteTakesBytesFromTheAbsoluteAddress can assert exactly how far the
// executor got.
constexpr std::size_t kBenchConnectWrites = 11;
constexpr std::size_t kEraseWrites = 2;

// Every primary request carries the 4-byte big-endian 0x7E0 envelope; every
// primary response the 0x7E8 reply id (legacy connect_bootloader, lines
// 110-113).
bytes::Bytes requestTo(std::uint32_t id, bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
bytes::Bytes requestTo(std::uint32_t id, std::initializer_list<bytes::Byte> payload)
{
    return requestTo(id, bytes::ByteView(payload.begin(), payload.size()));
}
bytes::Bytes request(bytes::ByteView payload)
{
    return requestTo(0x7e0, payload);
}
bytes::Bytes request(std::initializer_list<bytes::Byte> payload)
{
    return requestTo(0x7e0, payload);
}
bytes::Bytes responseFrom(std::uint32_t id, std::initializer_list<bytes::Byte> tail)
{
    return requestTo(id, tail);
}
bytes::Bytes response(std::initializer_list<bytes::Byte> tail)
{
    return requestTo(0x7e8, tail);
}

fastecu::flash::FlashPlan readPlan()
{
    auto plan = build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

fastecu::flash::FlashPlan writePlan(bytes::Bytes rom)
{
    auto plan = build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Write, kProtocol, kMcu, std::move(rom));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// Hand-built rather than produced by build_subaru_denso_sh72543_can_diesel_plan,
// so a plan whose operation the builder itself would refuse can still reach the
// executor -- the only way to prove the executor's own
// validate_subaru_denso_sh72543_can_diesel_plan call rejects it before any I/O.
fastecu::flash::FlashPlan handBuiltPlan(FlashOperation operation)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::SubaruDensoSh72543CanDiesel;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = std::string(kProtocol);
    fields.mcu_name = std::string(kMcu);
    fields.transfer_region = fastecu::flash::MemoryRegion{kBlockStart, kBlockLength};
    fields.erase_regions = {fastecu::flash::MemoryRegion{kBlockStart, kBlockLength}};
    fields.image = bytes::Bytes(kImageSize, 0x00);
    fields.family_plan = SubaruDensoSh72543CanDieselPlan{0x7e0, 0x7e8, 500000, false, 0x8000, 0x100};
    auto plan = fastecu::flash::validate_and_build(std::move(fields));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// The seed/encrypt tables, transcribed independently from the same legacy
// lines the executor was (generate_can_seed_key/encrypt_payload/
// decrypt_payload, lines 1495-1550) rather than read back from the executor's
// own translation unit, so a wrong table entry in the executor fails these
// assertions instead of passing silently.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
                                                      0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0xC85B, 0x32C0, 0xE282, 0x92A0};
constexpr std::array<std::uint8_t, 32> kIndexTransformation{0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                            0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                            0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

bytes::Bytes seedKey(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable, kIndexTransformation);
}

// The executor's decrypt table is this encrypt table exactly reversed, and
// SsmProtocol::calculatePayload's Feistel structure inverts by reversing key
// order, so this single helper both (a) pre-encrypts a known plaintext into
// the wire bytes a scripted read reply must carry for the executor's decrypt
// step to recover it, and (b) computes the wire bytes a write must carry for
// a known plaintext image.
bytes::Bytes toWire(bytes::ByteView plain)
{
    return SsmProtocol::calculatePayload(plain, static_cast<std::uint32_t>(plain.size()), kEncryptTable,
                                         kIndexTransformation);
}

const bytes::Bytes kSeed{0x11, 0x22, 0x33, 0x44};

// The OBK probe miss, the four non-fatal identity queries, the access-method
// probe and the branch selector (legacy lines 108-341). Byte 7 of the raw
// 0x22 0x10 0x1D reply frame -- payload index 3 -- selects the programming
// branch at line 342. Unlike its three siblings, this family asks for the ECU
// id with ReadDataByIdentifier 0xF182 and gets 62 F1 82 back (lines 133-146),
// not the vendor 0xAA/0xEA pair.
void scriptPreliminaries(ScriptedCanFlashTransport& t, bytes::Byte branchByte)
{
    t.expectWrite(request({0x10, 0x5F})); // OBK probe (lines 108-128), miss
    t.queueRead(response({0x50, 0x01}));
    t.expectWrite(request({0x22, 0xF1, 0x82})); // ECU ID (lines 131-173)
    t.queueRead(response({0x62, 0xF1, 0x82, 'I', 'D'}));
    t.expectWrite(request({0x09, 0x02})); // VIN (lines 175-206)
    t.queueRead(response({0x49, 0x02, 'V', 'I', 'N'}));
    t.expectWrite(request({0x09, 0x04})); // CAL ID (lines 208-243)
    t.queueRead(response({0x49, 0x04, 'C', 'A', 'L'}));
    t.expectWrite(request({0x09, 0x06})); // CVN (lines 245-282)
    t.queueRead(response({0x49, 0x06, 0xAA, 0xBB}));
    t.expectWrite(request({0x10, 0x5F})); // access method (lines 284-312)
    t.queueRead(response({0x50, 0x01}));
    t.expectWrite(request({0x22, 0x10, 0x1D})); // branch selector (lines 314-342)
    t.queueRead(response({0x62, 0x10, 0x1D, branchByte}));
}

// The bench arm (legacy lines 649-791). Its kernel jump reads once before the
// loop (line 770), where the SH72531 sibling reads twice.
void scriptBenchConnect(ScriptedCanFlashTransport& t)
{
    scriptPreliminaries(t, 0xFF);
    t.expectWrite(request({0x10, 0x43})); // lines 655-681
    t.queueRead(response({0x50, 0x43}));
    t.expectWrite(request({0x27, 0x61})); // lines 684-710
    t.queueRead(response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    bytes::Bytes key{0x27, 0x62}; // lines 722-753
    const bytes::Bytes k = seedKey(kSeed);
    key.insert(key.end(), k.begin(), k.end());
    t.expectWrite(request(key));
    t.queueRead(response({0x67, 0x62}));
    t.expectWrite(request({0x10, 0x42})); // lines 758-790
    t.queueRead(response({0x50, 0x42}));
}

// The 0x34/0x35 dump setup pair (legacy read_memory, lines 826-901). This
// family computes the address and length bytes from its region (lines 835-843)
// rather than emitting them as literals; the resulting wire bytes are what is
// scripted here.
void scriptReadSetup(ScriptedCanFlashTransport& t)
{
    t.expectWrite(request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}));
    t.queueRead(response({0x74, 0x20, 0x01, 0x05}));
    t.expectWrite(request({0x35, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}));
    t.queueRead(response({0x75, 0x20, 0x01, 0x01}));
}

// The chunked 0xB7 dump sweep (legacy read_memory, lines 907-1012): 0xB7 plus
// a 4-byte big-endian address, answered with 0xF7 plus one encrypted page.
void scriptFlashDump(ScriptedCanFlashTransport& t, std::uint32_t start, std::uint32_t length, std::uint32_t pagesize,
                     bytes::Byte fill)
{
    const bytes::Bytes wirePage = toWire(bytes::Bytes(pagesize, fill));
    for (std::uint32_t addr = start; addr < start + length; addr += pagesize)
    {
        t.expectWrite(request(bytes::composeBe(bytes::Byte(0xB7), addr)));
        bytes::Bytes reply = response({0xF7});
        reply.insert(reply.end(), wirePage.begin(), wirePage.end());
        t.queueRead(reply);
    }
}

// The 0x37 stop command (legacy read_memory, lines 1023-1045).
void scriptStopCommand(ScriptedCanFlashTransport& t)
{
    t.expectWrite(request({0x37}));
    t.queueRead(response({0x77}));
}

// The in-car arm (legacy lines 342-649). The ten fire-and-forget replies are
// deliberately given arbitration ids other than 0x7E8 wherever the addressed
// module would answer on its own id: legacy reads whichever frame arrives
// next without checking the id, and this pins that the port does not add a
// check legacy lacks.
void scriptInCarConnect(ScriptedCanFlashTransport& t)
{
    scriptPreliminaries(t, 0x00);

    t.expectWrite(request({0x10, 0x5F})); // lines 346-372, mismatch logs only
    t.queueRead(response({0x50, 0x01}));

    t.expectWrite(requestTo(0x7A2, {0x10, 0xC0})); // lines 375-383
    t.queueRead(responseFrom(0x7AA, {0x50, 0xC0}));
    t.expectWrite(request({0x10, 0x63})); // lines 386-394
    t.queueRead(response({0x50, 0x63}));
    t.expectWrite(requestTo(0x7DF, {0x10, 0x03})); // lines 397-405
    t.queueRead(response({0x50, 0x03}));
    t.expectWrite(requestTo(0x7E1, {0x10, 0x03})); // lines 408-416
    t.queueRead(responseFrom(0x7E9, {0x50, 0x03}));
    t.expectWrite(requestTo(0x7B0, {0x10, 0x03})); // lines 419-427
    t.queueRead(responseFrom(0x7B8, {0x50, 0x03}));
    t.expectWrite(requestTo(0x7B0, {0x85, 0x02})); // lines 430-438
    t.queueRead(responseFrom(0x7B8, {0xC5, 0x02}));
    t.expectWrite(requestTo(0x7DF, {0x85, 0x02})); // lines 441-449
    t.queueRead(response({0xC5, 0x02}));
    t.expectWrite(requestTo(0x7B0, {0x85, 0x02})); // lines 452-460
    t.queueRead(responseFrom(0x7B8, {0xC5, 0x02}));
    t.expectWrite(requestTo(0x7DF, {0x85, 0x02})); // lines 463-471
    t.queueRead(response({0xC5, 0x02}));
    t.expectWrite(requestTo(0x7DF, {0x28, 0x03, 0x01})); // lines 474-483
    t.queueRead(response({0x68, 0x03}));

    t.expectWrite(request({0x27, 0x61})); // lines 485-515
    t.queueRead(response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    bytes::Bytes key{0x27, 0x62}; // lines 524-556
    const bytes::Bytes k = seedKey(kSeed);
    key.insert(key.end(), k.begin(), k.end());
    t.expectWrite(request(key));
    t.queueRead(response({0x67, 0x62}));

    t.expectWrite(request({0x10, 0x5F})); // lines 558-585, fatal on mismatch
    t.queueRead(response({0x50, 0x63}));
    t.expectWrite(request({0x22, 0x10, 0x1D})); // lines 587-615, fatal on mismatch
    t.queueRead(response({0x62, 0x10, 0x1D, 0x00}));
    t.expectWrite(request({0x10, 0x62})); // lines 619-648
    t.queueRead(response({0x50, 0x62}));
}

// erase_memory's setup PDU plus its erase trigger (legacy lines 1396-1450);
// the trigger's answer is consumed by the re-read loop, not by a paired read.
void scriptEraseMemory(ScriptedCanFlashTransport& t)
{
    t.expectWrite(request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}));
    t.queueRead(response({0x74, 0x20, 0x01, 0x05}));
    t.expectWrite(request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

// The 0xB6 write-chunk sweep for block 0 (legacy reflash_block, lines
// 1195-1249). `rom` is the whole 0x200000 plan image, encrypted once, and
// indexed from kImageStart = 0 -- so chunk 0 carries encrypted[0x8000..0x8100).
void scriptReflashChunks(ScriptedCanFlashTransport& t, bytes::ByteView rom)
{
    const bytes::Bytes encrypted = toWire(rom);
    for (std::uint32_t offset = 0; offset < kBlockLength; offset += 256)
    {
        const std::uint32_t addr = kBlockStart + offset;
        t.expectWrite(request(
            bytes::composeBe(bytes::Byte(0xB6), addr, bytes::ByteView(encrypted).subspan(addr - kImageStart, 256))));
        t.queueRead(response({0xF6}));
    }
}

// The close-block 0x37 and the checksum verify (legacy lines 1282-1365).
// UdsClient absorbs the intermediate 0x78 responsePending NRC by re-reading,
// so only one write is expected even though two reads are queued.
void scriptCloseAndChecksum(ScriptedCanFlashTransport& t)
{
    t.expectWrite(request({0x37}));
    t.queueRead(response({0x77}));
    t.expectWrite(request({0x31, 0x01, 0x02, 0x02, 0x01}));
    t.queueRead(response({0x7F, 0x31, 0x78}));
    t.queueRead(response({0x71, 0x01, 0x02}));
}

bytes::Bytes writeRom()
{
    bytes::Bytes rom(kImageSize, 0x00);
    for (std::size_t i = 0; i < rom.size(); ++i)
    {
        rom[i] = static_cast<bytes::Byte>(i & 0xff);
    }
    return rom;
}

TEST(SubaruDensoSh72543CanDieselExecutor, BenchReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    scriptReadSetup(transport);
    scriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    scriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& rom = *result->read_bytes;
    EXPECT_EQ(rom.size(), kImageSize);
    EXPECT_THAT(bytes::ByteView(rom).first(0x8000), Each(0xFF));                 // leading pad
    EXPECT_THAT(bytes::ByteView(rom).subspan(0x8000, kBlockLength), Each(0xA5)); // decrypted payload
    EXPECT_THAT(bytes::ByteView(rom).last(0x100), Each(0xFF));                   // tail pad
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info,
                                           "Connecting to ECU Denso SH72543 Diesel CAN bootloader, please wait...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Reading ROM from ECU, Denso SH72543 Diesel using CAN")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "OBK not active, initialising ECU...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Bench programming: accessing, please wait...")));
    // Legacy's own wording for this line, lowercase-hex and unpadded because
    // it is built with QString::number(x, 16) (lines 822-824).
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Setting flash start: 0x8000 and length: 0x1f7f00")));
}

TEST(SubaruDensoSh72543CanDieselExecutor, InCarReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    scriptInCarConnect(transport);
    scriptReadSetup(transport);
    scriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    scriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& rom = *result->read_bytes;
    EXPECT_EQ(rom.size(), kImageSize);
    EXPECT_THAT(bytes::ByteView(rom).first(0x8000), Each(0xFF));
    EXPECT_THAT(bytes::ByteView(rom).subspan(0x8000, kBlockLength), Each(0x5A));
    EXPECT_THAT(bytes::ByteView(rom).last(0x100), Each(0xFF));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "In car programming: accessing, please wait...")));
}

TEST(SubaruDensoSh72543CanDieselExecutor, WriteErasesThenFlashesBlockZero)
{
    ScriptedCanFlashTransport transport;
    const bytes::Bytes rom = writeRom();

    scriptBenchConnect(transport);
    scriptEraseMemory(transport);
    transport.queueRead(response({0x71, 0x01, 0x02}));
    scriptReflashChunks(transport, rom);
    scriptCloseAndChecksum(transport);

    RecordingClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(writePlan(rom), transport, clock, cancellation, events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.notices, Contains("Writing ROM, please wait..."));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Writing ROM to ECU, Denso SH72543 Diesel using CAN")));
    // The block index legacy reports, which is 0 for this family's
    // single-block table (lines 1092-1101, 1138).
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Blocks to flash: 0,  (total: 1)")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Block 0 reflash complete.")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Flash erased! Starting flash write, do not power off!")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Closing out Flashing of this block")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Checksum verified")));
    // Every sleep the write path performs, in order, each with the legacy
    // delay() it reproduces: connect_bench's wait (line 653, 50 ms here where
    // the three sibling families wait 500), the settle after the erase command
    // (line 1452), and the settle before the checksum-verify write (line
    // 1313). Asserted as a whole sequence rather than by Contains so that
    // dropping one fails here instead of passing silently.
    EXPECT_EQ(clock.sleep_calls, (std::vector<int>{50, 500, 100}));
}

TEST(SubaruDensoSh72543CanDieselExecutor, WriteTakesBytesFromTheAbsoluteAddress)
{
    // A 0x200000 image whose byte at 0x8000 is 0xC3 must put 0xC3 in the first
    // written chunk -- not the byte at offset 0. Legacy composes the same
    // indexing out of two halves: write_memory offsets the encrypted buffer by
    // fblocks[0].start = 0x8000 (line 1129) and reflash_block then indexes it
    // block-relative (line 1217), which together address
    // encrypted[block_addr + i]. Only the first 0xB6 chunk is scripted, so the
    // executor stops on the second with an "unexpected scripted CAN write";
    // the write count is what pins the base -- an image based at 0x8000 would
    // have failed one write earlier, on the first chunk.
    bytes::Bytes rom(kImageSize, 0x00);
    rom[0x8000] = 0xC3;

    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    scriptEraseMemory(transport);
    transport.queueRead(response({0x71, 0x01, 0x02}));
    const bytes::Bytes encrypted = toWire(rom);
    transport.expectWrite(request(bytes::composeBe(
        bytes::Byte(0xB6), kBlockStart, bytes::ByteView(encrypted).subspan(kBlockStart - kImageStart, 256))));
    transport.queueRead(response({0xF6}));

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(writePlan(rom), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_EQ(transport.writesConsumed(), kBenchConnectWrites + kEraseWrites + 1);
    EXPECT_TRUE(transport.scriptConsumed());
    // Independent of the executor: the planted byte really does distinguish
    // the two candidate bases, so the assertion above cannot pass vacuously.
    EXPECT_NE(toWire(bytes::ByteView(rom).subspan(0x8000, 256)), toWire(bytes::ByteView(rom).first(256)));
}

TEST(SubaruDensoSh72543CanDieselExecutor, TestWriteIsRejectedBeforeAnyTransportCall)
{
    // Legacy threaded test_write from execute() through write_memory into
    // reflash_block and never consulted it, so a test_write run performed a
    // real erase and a real 0xB6 flash write. The port refuses before it
    // configures or opens the transport, let alone reaches the ECU.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(handBuiltPlan(FlashOperation::TestWrite), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_FALSE(transport.last_config_.has_value());
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(SubaruDensoSh72543CanDieselExecutor, BenchSessionMismatchIsToleratedAndContinues)
{
    // Unique to this family: the bench arm's `10 43` check carries a
    // commented-out `return STATUS_ERROR` (line 672), so a wrong `50 43`
    // answer is logged and stepped over where all three sibling families
    // abort. The connect must therefore reach the seed request, which is the
    // next write scripted below.
    ScriptedCanFlashTransport transport;
    scriptPreliminaries(transport, 0xFF);
    transport.expectWrite(request({0x10, 0x43}));
    transport.queueRead(response({0x50, 0x01})); // wrong subfunction, tolerated
    transport.expectWrite(request({0x27, 0x61}));
    transport.queue_error(ErrorKind::Timeout, "stop after the tolerated mismatch");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
    // The mismatch was logged, not swallowed silently...
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Error, "Wrong response from ECU: 50 01 ")));
    // ...and the sequence carried on past it into the seed request.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Starting seed request")));
}

TEST(SubaruDensoSh72543CanDieselExecutor, ReadTimeoutPropagates)
{
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    transport.expectWrite(request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}));
    transport.queue_error(ErrorKind::Timeout, "no reply");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
    // The bench arm's own 50 ms wait (line 653) is the only sleep the whole
    // connect performs: the kernel jump is acknowledged on the loop's first
    // look, so its 100 ms retry sleep (line 784) is never reached.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Kernel jump acknowledged")));
    EXPECT_EQ(clock.now_, 50u);
}

TEST(SubaruDensoSh72543CanDieselExecutor, ReadDisconnectPropagates)
{
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    scriptReadSetup(transport);
    transport.expectWrite(request(bytes::composeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh72543CanDieselExecutor, NegativeResponseDuringConnectFails)
{
    // The seed request (legacy lines 684-710) is one of the exchanges this
    // family does abort on: a negative response must fail rather than be
    // logged and stepped over, unlike the `10 43` session above it.
    ScriptedCanFlashTransport transport;
    scriptPreliminaries(transport, 0xFF);
    transport.expectWrite(request({0x10, 0x43}));
    transport.queueRead(response({0x50, 0x43}));
    transport.expectWrite(request({0x27, 0x61}));
    transport.queueRead(response({0x7F, 0x27, 0x35}));

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

// Cancels the token as soon as the first dump page's progress is reported,
// mirroring subaru_hitachi_m32r_can_executor_test.cpp's own sink.
class CancelAfterFirstPageSink final : public RecordingEventSink
{
  public:
    explicit CancelAfterFirstPageSink(fastecu::ManualCancellationToken& source) : source_(source)
    {
    }
    void phase_progress(const fastecu::PhaseProgressEvent& event) override
    {
        RecordingEventSink::phase_progress(event);
        if (event.phase_name == "Read ROM" && event.done > 0)
        {
            source_.cancel();
        }
    }

  private:
    fastecu::ManualCancellationToken& source_;
};

TEST(SubaruDensoSh72543CanDieselExecutor, CancellationMidReadReturnsCancelled)
{
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    scriptReadSetup(transport);
    // Exactly one page is scripted; the executor is cancelled while it is
    // being served, so the sweep must stop at the top of the next page.
    scriptFlashDump(transport, kBlockStart, kPageSize, kPageSize, 0x00);

    FakeClock clock;
    fastecu::ManualCancellationToken cancellation;
    CancelAfterFirstPageSink events{cancellation};
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh72543CanDieselExecutor, EmptyBranchSelectorReplyFails)
{
    // Legacy lines 333-338: an absent `22 10 1D` reply is one of only two
    // points in the preliminary phase that return STATUS_ERROR.
    ScriptedCanFlashTransport transport;
    transport.expectWrite(request({0x10, 0x5F}));
    transport.queueRead(response({0x50, 0x01}));
    transport.expectWrite(request({0x22, 0xF1, 0x82}));
    transport.queueRead(response({0x62, 0xF1, 0x82, 'I', 'D'}));
    transport.expectWrite(request({0x09, 0x02}));
    transport.queueRead(response({0x49, 0x02, 'V', 'I', 'N'}));
    transport.expectWrite(request({0x09, 0x04}));
    transport.queueRead(response({0x49, 0x04, 'C', 'A', 'L'}));
    transport.expectWrite(request({0x09, 0x06}));
    transport.queueRead(response({0x49, 0x06, 0xAA, 0xBB}));
    transport.expectWrite(request({0x10, 0x5F}));
    transport.queueRead(response({0x50, 0x01}));
    transport.expectWrite(request({0x22, 0x10, 0x1D}));
    transport.queue_no_frame();

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh72543CanDieselExecutor, EraseRetryExhaustionFails)
{
    // Legacy erase_memory's re-read loop (lines 1456-1483): twenty reads, no
    // re-send, then "Flash area erase failed".
    ScriptedCanFlashTransport transport;
    const bytes::Bytes rom = writeRom();
    scriptBenchConnect(transport);
    scriptEraseMemory(transport);
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        transport.queueRead(response({0x71, 0x01, 0x03}));
    }

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    auto result = executor.execute(writePlan(rom), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

} // namespace
