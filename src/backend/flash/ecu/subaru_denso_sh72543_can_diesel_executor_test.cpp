// Equivalence tests for SubaruDensoSh72543CanDieselExecutor, the portable
// replacement for flash_ecu_subaru_denso_sh72543_can_diesel_operation.cpp.
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/ecu/testing/can_executor_conformance.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::flash::build_subaru_denso_sh72543_can_diesel_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruDensoSh72543CanDieselExecutor;
using fastecu::flash::SubaruDensoSh72543CanDieselPlan;
// INSTANTIATE_TYPED_TEST_SUITE_P token-pastes its generated names against
// whatever namespace is visible unqualified at the call site, so
// CanExecutorConformance's must be brought in wholesale rather than by a
// single using-declaration.
using namespace fastecu::flash::testing;
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
    fastecu::Status sleep(std::chrono::milliseconds duration, const fastecu::ICancellationToken& cancellation) override
    {
        sleep_calls.push_back(duration);
        return FakeClock::sleep(duration, cancellation);
    }

    std::vector<std::chrono::milliseconds> sleep_calls;
};

constexpr std::string_view kProtocol = "sub_ecu_denso_sh72543_can_diesel";
constexpr std::string_view kMcu = "SH72543d";

// fblocks_SH72543d is the wave's only single-block table: [0] = {0x00008000,
// 0x1F7F00}, with the 0x0-0x8000 entry commented out. The image is still based
// at address 0 -- read_memory prepends 0x8000 of 0xFF, and write_memory
// offsets its buffer pointer by fblocks[0].start before reflash_block indexes
// it block-relative.
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
// primary response the 0x7E8 reply id (legacy connect_bootloader).
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

fastecu::Result<fastecu::flash::FlashPlan> readPlan()
{
    return build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
}

fastecu::Result<fastecu::flash::FlashPlan> writePlan(bytes::Bytes rom)
{
    return build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Write, kProtocol, kMcu, std::move(rom));
}

// Hand-built rather than produced by build_subaru_denso_sh72543_can_diesel_plan,
// so a plan whose operation the builder itself would refuse can still reach the
// executor -- the only way to prove the executor's own
// validate_subaru_denso_sh72543_can_diesel_plan call rejects it before any I/O.
fastecu::Result<fastecu::flash::FlashPlan> handBuiltPlan(FlashOperation operation)
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
    return fastecu::flash::validate_and_build(std::move(fields));
}

// The seed/encrypt tables, transcribed independently from the same legacy
// lines the executor was
// (generate_can_seed_key/encrypt_payload/decrypt_payload) rather than read
// back from the executor's own translation unit, so a wrong table entry in the
// executor fails these assertions instead of passing silently.
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
// probe and the branch selector. Byte 7 of the raw 0x22 0x10 0x1D reply frame
// -- payload index 3 -- selects the programming branch. Unlike its three
// siblings, this family asks for the ECU id with ReadDataByIdentifier 0xF182
// and gets 62 F1 82 back, not the vendor 0xAA/0xEA pair.
void scriptPreliminaries(ScriptedCanFlashTransport& t, bytes::Byte branchByte)
{
    const auto section = t.section("preliminaries");
    t.exchange(request({0x10, 0x5F}), response({0x50, 0x01}));                         // OBK probe, miss
    t.exchange(request({0x22, 0xF1, 0x82}), response({0x62, 0xF1, 0x82, 'I', 'D'}));   // ECU ID
    t.exchange(request({0x09, 0x02}), response({0x49, 0x02, 'V', 'I', 'N'}));          // VIN
    t.exchange(request({0x09, 0x04}), response({0x49, 0x04, 'C', 'A', 'L'}));          // CAL ID
    t.exchange(request({0x09, 0x06}), response({0x49, 0x06, 0xAA, 0xBB}));             // CVN
    t.exchange(request({0x10, 0x5F}), response({0x50, 0x01}));                         // access method
    t.exchange(request({0x22, 0x10, 0x1D}), response({0x62, 0x10, 0x1D, branchByte})); // branch selector
}

// The bench arm. Its kernel jump reads once before the loop, where the SH72531
// sibling reads twice.
void scriptBenchConnect(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("bench connect");
    scriptPreliminaries(t, 0xFF);
    t.exchange(request({0x10, 0x43}), response({0x50, 0x43}));
    t.exchange(request({0x27, 0x61}), response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    bytes::Bytes key{0x27, 0x62};
    const bytes::Bytes k = seedKey(kSeed);
    key.insert(key.end(), k.begin(), k.end());
    t.exchange(request(key), response({0x67, 0x62}));
    t.exchange(request({0x10, 0x42}), response({0x50, 0x42}));
}

// The 0x34/0x35 dump setup pair (legacy read_memory). This family computes the
// address and length bytes from its region rather than emitting them as
// literals; the resulting wire bytes are what is scripted here.
void scriptReadSetup(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("read setup");
    t.exchange(request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}),
               response({0x74, 0x20, 0x01, 0x05}));
    t.exchange(request({0x35, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}),
               response({0x75, 0x20, 0x01, 0x01}));
}

// The chunked 0xB7 dump sweep (legacy read_memory): 0xB7 plus a 4-byte big-
// endian address, answered with 0xF7 plus one encrypted page.
void scriptFlashDump(ScriptedCanFlashTransport& t, std::uint32_t start, std::uint32_t length, std::uint32_t pagesize,
                     bytes::Byte fill)
{
    const auto section = t.section("flash dump");
    const bytes::Bytes wirePage = toWire(bytes::Bytes(pagesize, fill));
    for (std::uint32_t addr = start; addr < start + length; addr += pagesize)
    {
        bytes::Bytes reply = response({0xF7});
        reply.insert(reply.end(), wirePage.begin(), wirePage.end());
        t.exchange(request(bytes::composeBe(bytes::Byte(0xB7), addr)), reply);
    }
}

// The 0x37 stop command (legacy read_memory).
void scriptStopCommand(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("stop command");
    t.exchange(request({0x37}), response({0x77}));
}

// Connect plus read_memory's 0x34 dump-setup request, registered as an
// expected write only -- its reply is left for the caller to queue, so the
// same script serves every "the next read fails" conformance test
// (fastecu::flash::testing::CanExecutorConformance) regardless of which
// failure mode (a transport error, a bare timeout, or an empty frame) belongs
// there. Unlike its 1N83M 4M sibling, this family keeps read_memory's
// 0x34/0x35 setup checks live (NegativeResponseAtDumpSetupFails), so this
// point is safe to use here.
void scriptUpToFirstFatalRead(ScriptedCanFlashTransport& t)
{
    scriptBenchConnect(t);
    const auto section = t.section("read setup (first request only)");
    t.exchange(request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}));
}

// The in-car arm. The ten fire-and-forget replies are deliberately given
// arbitration ids other than 0x7E8 wherever the addressed module would answer
// on its own id: legacy reads whichever frame arrives next without checking
// the id, and this pins that the port does not add a check legacy lacks.
void scriptInCarConnect(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("in-car connect");
    scriptPreliminaries(t, 0x00);

    t.exchange(request({0x10, 0x5F}), response({0x50, 0x01})); // mismatch logs only

    t.exchange(requestTo(0x7A2, {0x10, 0xC0}), responseFrom(0x7AA, {0x50, 0xC0}));
    t.exchange(request({0x10, 0x63}), response({0x50, 0x63}));
    t.exchange(requestTo(0x7DF, {0x10, 0x03}), response({0x50, 0x03}));
    t.exchange(requestTo(0x7E1, {0x10, 0x03}), responseFrom(0x7E9, {0x50, 0x03}));
    t.exchange(requestTo(0x7B0, {0x10, 0x03}), responseFrom(0x7B8, {0x50, 0x03}));
    t.exchange(requestTo(0x7B0, {0x85, 0x02}), responseFrom(0x7B8, {0xC5, 0x02}));
    t.exchange(requestTo(0x7DF, {0x85, 0x02}), response({0xC5, 0x02}));
    t.exchange(requestTo(0x7B0, {0x85, 0x02}), responseFrom(0x7B8, {0xC5, 0x02}));
    t.exchange(requestTo(0x7DF, {0x85, 0x02}), response({0xC5, 0x02}));
    t.exchange(requestTo(0x7DF, {0x28, 0x03, 0x01}), response({0x68, 0x03}));

    t.exchange(request({0x27, 0x61}), response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    bytes::Bytes key{0x27, 0x62};
    const bytes::Bytes k = seedKey(kSeed);
    key.insert(key.end(), k.begin(), k.end());
    t.exchange(request(key), response({0x67, 0x62}));

    t.exchange(request({0x10, 0x5F}), response({0x50, 0x63}));                   // fatal on mismatch
    t.exchange(request({0x22, 0x10, 0x1D}), response({0x62, 0x10, 0x1D, 0x00})); // fatal on mismatch
    t.exchange(request({0x10, 0x62}), response({0x50, 0x62}));
}

// erase_memory's setup PDU plus its erase trigger; the trigger's answer is
// consumed by the re-read loop, not by a paired read.
void scriptEraseMemory(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("erase memory");
    t.exchange(request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}),
               response({0x74, 0x20, 0x01, 0x05}));
    t.exchange(request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

// The 0xB6 write-chunk sweep for block 0 (legacy reflash_block). `rom` is the
// whole 0x200000 plan image, encrypted once, and indexed from kImageStart = 0
// -- so chunk 0 carries encrypted[0x8000..0x8100).
void scriptReflashChunks(ScriptedCanFlashTransport& t, bytes::ByteView rom)
{
    const auto section = t.section("reflash chunks");
    const bytes::Bytes encrypted = toWire(rom);
    for (std::uint32_t offset = 0; offset < kBlockLength; offset += 256)
    {
        const std::uint32_t addr = kBlockStart + offset;
        t.exchange(request(bytes::composeBe(bytes::Byte(0xB6), addr,
                                            bytes::ByteView(encrypted).subspan(addr - kImageStart, 256))),
                   response({0xF6}));
    }
}

// The close-block 0x37 and the checksum verify. UdsClient absorbs the
// intermediate 0x78 responsePending NRC by re-reading, so only one write is
// expected even though two reads are queued.
void scriptCloseAndChecksum(ScriptedCanFlashTransport& t)
{
    const auto section = t.section("close and checksum");
    t.exchange(request({0x37}), response({0x77}));
    t.exchange(request({0x31, 0x01, 0x02, 0x02, 0x01}), response({0x7F, 0x31, 0x78}));
    t.queueRead(response({0x71, 0x01, 0x02}));
}

bytes::Bytes writeRom()
{
    bytes::Bytes rom(kImageSize, 0x00);
    for (std::size_t i = 0; i < rom.size(); ++i)
    {
        rom[i] = static_cast<bytes::Byte>(i);
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

    const auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.execute(*plan, transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
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
    // it is built with QString::number(x, 16).
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

    const auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.execute(*plan, transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
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

    const auto plan = writePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.execute(*plan, transport, clock, cancellation, events);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.notices, Contains("Writing ROM, please wait..."));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Writing ROM to ECU, Denso SH72543 Diesel using CAN")));
    // The block index legacy reports, which is 0 for this family's single-
    // block table.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Blocks to flash: 0,  (total: 1)")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Block 0 reflash complete.")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Flash erased! Starting flash write, do not power off!")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Closing out Flashing of this block")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Checksum verified")));
    // Every sleep the write path performs, in order, each with the legacy
    // delay() it reproduces: connect_bench's wait (50 ms here, where the three
    // sibling families wait 500), the settle after the erase command, and the
    // settle before the checksum-verify write. Asserted as a whole sequence
    // rather than by Contains so that dropping one fails here instead of
    // passing silently.
    EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{50ms, 500ms, 100ms}));
}

TEST(SubaruDensoSh72543CanDieselExecutor, WriteTakesBytesFromTheAbsoluteAddress)
{
    // A 0x200000 image whose byte at 0x8000 is 0xC3 must put 0xC3 in the first
    // written chunk -- not the byte at offset 0. Legacy composes the same
    // indexing out of two halves: write_memory offsets the encrypted buffer by
    // fblocks[0].start = 0x8000 and reflash_block then indexes it block-
    // relative, which together address encrypted[block_addr + i]. Only the
    // first 0xB6 chunk is scripted, so the executor stops on the second with
    // an "unexpected scripted CAN write"; the write count is what pins the
    // base -- an image based at 0x8000 would have failed one write earlier, on
    // the first chunk.
    bytes::Bytes rom(kImageSize, 0x00);
    rom[0x8000] = 0xC3;

    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    scriptEraseMemory(transport);
    transport.queueRead(response({0x71, 0x01, 0x02}));
    const bytes::Bytes encrypted = toWire(rom);
    transport.exchange(request(bytes::composeBe(bytes::Byte(0xB6), kBlockStart,
                                                bytes::ByteView(encrypted).subspan(kBlockStart - kImageStart, 256))),
                       response({0xF6}));

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    const auto plan = writePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Internal));
    EXPECT_EQ(transport.writesConsumed(), kBenchConnectWrites + kEraseWrites + 1);
    EXPECT_TRUE(transport.scriptConsumed());
    // Independent of the executor: the planted byte really does distinguish
    // the two candidate bases, so the assertion above cannot pass vacuously.
    EXPECT_NE(toWire(bytes::ByteView(rom).subspan(0x8000, 256)), toWire(bytes::ByteView(rom).first(256)));
}

TEST(SubaruDensoSh72543CanDieselExecutor, BenchSessionMismatchIsToleratedAndContinues)
{
    // Unique to this family: the bench arm's `10 43` check carries a
    // commented-out `return STATUS_ERROR`, so a wrong `50 43` answer is logged
    // and stepped over where all three sibling families abort. The connect
    // must therefore reach the seed request, which is the next write scripted
    // below.
    ScriptedCanFlashTransport transport;
    scriptPreliminaries(transport, 0xFF);
    transport.exchange(request({0x10, 0x43}), response({0x50, 0x01})); // wrong subfunction, tolerated
    transport.exchange(request({0x27, 0x61}));
    transport.queue_error(ErrorKind::Timeout, "stop after the tolerated mismatch");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    const auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
    // The mismatch was logged, not swallowed silently...
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Error, "Wrong response from ECU: 50 01 ")));
    // ...and the sequence carried on past it into the seed request.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Starting seed request")));
}

// Kept alongside the generic conformance ReadTimeoutPropagates (which uses
// the same scriptUpToFirstFatalRead point) because this pins facts that test
// does not: the bench arm's own connect-timing shape. Its kernel jump reads
// only once (unlike the SH72531 sibling's two), so the connect's only sleep
// is the bench arm's 50 ms wait -- the kernel jump's 100 ms retry sleep is
// never reached because the single read is acknowledged immediately.
TEST(SubaruDensoSh72543CanDieselExecutor, ReadTimeoutAfterConnectOnlySleepsTheBenchWait)
{
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    transport.exchange(request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}));
    transport.queue_error(ErrorKind::Timeout, "no reply");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    const auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Kernel jump acknowledged")));
    EXPECT_EQ(clock.elapsed(), 50ms);
}

// Unlike ReadPropagatesADisconnectedTransport (can_executor_conformance.h),
// which stops at read_memory's 0x34 setup exchange -- a fatal_query call --
// this pins a transport error raised *inside* the 0xB7 dump-chunk loop, whose
// reads go through fatal_request at a different call site, inside a `for`
// loop carrying its own cancellation-check and progress-accumulation state.
// fatal_request's generic error propagation is covered once for every family
// by uds_client_exchange_common_test.cpp; this test is what actually proves
// the loop itself aborts cleanly -- without corrupting rom/progress state --
// on a transport error, rather than assuming fatal_request's coverage
// implies the loop wrapping it behaves the same way.
TEST(SubaruDensoSh72543CanDieselExecutor, ReadDisconnectMidDumpLoopPropagates)
{
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    scriptReadSetup(transport);
    transport.exchange(request(bytes::composeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    const auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh72543CanDieselExecutor, NegativeResponseDuringConnectFails)
{
    // The seed request is one of the exchanges this family does abort on: a
    // negative response must fail rather than be logged and stepped over,
    // unlike the `10 43` session above it.
    ScriptedCanFlashTransport transport;
    scriptPreliminaries(transport, 0xFF);
    transport.exchange(request({0x10, 0x43}), response({0x50, 0x43}));
    transport.exchange(request({0x27, 0x61}), response({0x7F, 0x27, 0x35}));

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    const auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh72543CanDieselExecutor, NegativeResponseAtDumpSetupFails)
{
    // The mirror image of subaru_denso_1n83m_4m_can_executor_test's
    // ProceedsPast... cases, and the same test the 1N83M 1.5M and SH72531
    // ports carry. This family keeps read_memory's 0x34/0x35 setup checks
    // live, so a negative answer must abort here where the 4M steps over it.
    // The pair of tests is what stops the difference being normalized away in
    // either direction. The scripted 0x34 bytes are computed from the region
    // rather than literal in this family -- see scriptReadSetup -- so they are
    // spelled out here the same way.
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    transport.exchange(request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}),
                       response({0x7F, 0x34, 0x31}));

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    const auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh72543CanDieselExecutor, EmptyBranchSelectorReplyFails)
{
    // an absent `22 10 1D` reply is one of only two points in the preliminary
    // phase that return STATUS_ERROR.
    ScriptedCanFlashTransport transport;
    transport.exchange(request({0x10, 0x5F}), response({0x50, 0x01}));
    transport.exchange(request({0x22, 0xF1, 0x82}), response({0x62, 0xF1, 0x82, 'I', 'D'}));
    transport.exchange(request({0x09, 0x02}), response({0x49, 0x02, 'V', 'I', 'N'}));
    transport.exchange(request({0x09, 0x04}), response({0x49, 0x04, 'C', 'A', 'L'}));
    transport.exchange(request({0x09, 0x06}), response({0x49, 0x06, 0xAA, 0xBB}));
    transport.exchange(request({0x10, 0x5F}), response({0x50, 0x01}));
    transport.exchange(request({0x22, 0x10, 0x1D}));
    transport.queue_no_frame();

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    const auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh72543CanDieselExecutor, EcuIdLogKeepsOnlyLegacysFiveBytes)
{
    // Legacy strips the 4-byte envelope and `62 F1 82` with remove(0, 7) and
    // leaves the remainder intact -- its remove(5, len-5) is commented out --
    // but the string it logs is built by `for (int i = 0; i < 5; i++)`, so a
    // longer reply is still logged as five bytes. Script eight and expect
    // five.
    ScriptedCanFlashTransport transport;
    transport.exchange(request({0x10, 0x5F}), response({0x50, 0x01}));
    transport.exchange(request({0x22, 0xF1, 0x82}),
                       response({0x62, 0xF1, 0x82, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}));
    transport.exchange(request({0x09, 0x02}));
    transport.queue_no_frame();

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    const auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "ECU ID: 11 22 33 44 55 ")));
}

TEST(SubaruDensoSh72543CanDieselExecutor, EraseRetryExhaustionFails)
{
    // Legacy erase_memory's re-read loop: twenty reads, no re-send, then
    // "Flash area erase failed".
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

    const auto plan = writePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
}

// Unlike this family's kProbeTimeout (2000ms, see the conformance suite's
// ConnectProbesReadWithThisFamilysProbeTimeout below), a full bench-connect +
// read-setup + full dump + stop-command run also reads with the plain short
// timeout (serial_read_short_timeout) exactly three times: the OBK probe and
// read_memory's own 0x34/0x35 setup pair. Kept as its own fact -- distinct
// from, not a restatement of, the family's probe timeout.
TEST(SubaruDensoSh72543CanDieselExecutor, ObkProbeAndReadSetupReadWithTheShortTimeout)
{
    using namespace std::chrono_literals;
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    scriptReadSetup(transport);
    scriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    scriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDensoSh72543CanDieselExecutor executor;

    const auto plan = readPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ASSERT_THAT(executor.execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_EQ(std::ranges::count(transport.readTimeouts(), 200ms), 3);
}

// The IFlashExecutor contract this family satisfies -- see
// can_executor_conformance.h. Every member forwards to a helper already
// defined above rather than reimplementing it, so the conformance suite
// exercises exactly the same scripts and plans the family's own local tests
// do.
struct Sh72543CanDieselTraits
{
    using Executor = SubaruDensoSh72543CanDieselExecutor;
    static constexpr fastecu::flash::Iso15765Config kWire{
        .bitrate = 500000, .request_id = 0x7e0, .response_id = 0x7e8, .extended_id = false};
    static constexpr std::uint32_t kBlockStart = ::kBlockStart;
    static constexpr std::uint32_t kBlockLength = ::kBlockLength;
    static constexpr std::uint32_t kPageSize = ::kPageSize;
    // connect_bootloader's tolerant_probe exchanges read with this family's
    // long timeout (serial_read_timeout), unlike its three siblings, which
    // probe with the short one.
    static constexpr std::chrono::milliseconds kProbeTimeout{2000};
    // Unlike its three siblings, this family's own read_memory dump-chunk
    // loop (the 0xB7 sweep) ALSO reads with kProbeTimeout (both are
    // kLongPolicy/serial_read_timeout), so this count is dominated by the
    // roughly 8063 dump-chunk reads (kBlockLength / kPageSize) rather than
    // by the handful of connect-time probes --
    // ObkProbeAndReadSetupReadWithTheShortTimeout above is what isolates the
    // small, human-checkable connect-time count instead.
    static constexpr int kProbeCount = 8073;

    static fastecu::Result<fastecu::flash::FlashPlan> readPlan()
    {
        return ::readPlan();
    }

    static fastecu::Result<fastecu::flash::FlashPlan> handBuiltPlan(FlashOperation operation)
    {
        return ::handBuiltPlan(operation);
    }

    static void scriptBenchConnect(ScriptedCanFlashTransport& t)
    {
        ::scriptBenchConnect(t);
    }

    static void scriptReadSetup(ScriptedCanFlashTransport& t)
    {
        ::scriptReadSetup(t);
    }

    static void scriptFlashDump(ScriptedCanFlashTransport& t, std::uint32_t start, std::uint32_t length,
                                std::uint32_t pagesize, bytes::Byte fill)
    {
        ::scriptFlashDump(t, start, length, pagesize, fill);
    }

    static void scriptStopCommand(ScriptedCanFlashTransport& t)
    {
        ::scriptStopCommand(t);
    }

    static void scriptUpToFirstFatalRead(ScriptedCanFlashTransport& t)
    {
        ::scriptUpToFirstFatalRead(t);
    }
};

INSTANTIATE_TYPED_TEST_SUITE_P(SubaruDensoSh72543CanDiesel, CanExecutorConformance, Sh72543CanDieselTraits);

} // namespace
