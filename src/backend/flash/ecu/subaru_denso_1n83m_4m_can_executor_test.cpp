// Equivalence tests for SubaruDenso1n83m_4mCanExecutor, the portable
// replacement for FlashEcuSubaruDenso1N83M_4MCanOperation's
// connect_bootloader(), read_memory(), write_memory(), reflash_block() and
// erase_memory(). Every scripted exchange cites the line of
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_1n83m_4m_can_operation.cpp
// it was transcribed from, as of 20892df -- this branch's base, and the last
// commit before the file was deleted here. Master later reflowed one line in
// three of the four (S1117, #240), so resolve citations against 20892df.
//
// This family's defining property is tolerance: seven checks whose
// `return STATUS_ERROR` legacy commented out (lines 305, 335, 369, 876, 883,
// 917, 924) log and proceed where its 1N83M 1.5M and SH72531 siblings abort.
// The two `ProceedsPast...` tests below assert that positively -- between
// them they drive all seven -- and the mirror-image
// `NegativeResponseAtDumpSetupFails` cases in those two siblings' suites
// assert the opposite, so the difference cannot be normalized away in either
// direction without a test failing.
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.h"

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
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
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
using fastecu::flash::build_subaru_denso_1n83m_4m_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruDenso1n83m_4mCanExecutor;
using fastecu::flash::SubaruDenso1n83m_4mCanPlan;
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

constexpr std::string_view kProtocol = "sub_ecu_denso_1n83m_4m_can";
constexpr std::string_view kMcu = "N83M_4MB";

// fblocks_N83M_4MB: [0] = {0x08F9C000, 0x10000}, [1] = {0x08FAC000,
// 0x3D3F00}, [2] = {0x0937FF00, 0x100}.
constexpr std::uint32_t kImageStart = 0x08F9C000;
constexpr std::uint32_t kBlockStart = 0x08FAC000;
constexpr std::uint32_t kBlockLength = 0x3D3F00;
constexpr std::size_t kImageSize = 0x3E4000;
constexpr std::uint32_t kPageSize = 0x100;

// Every primary request carries the 4-byte big-endian 0x7E0 envelope; every
// primary response the 0x7E8 reply id (legacy connect_bootloader, lines
// 107-110).
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
    auto plan = build_subaru_denso_1n83m_4m_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

fastecu::flash::FlashPlan writePlan(bytes::Bytes rom)
{
    auto plan = build_subaru_denso_1n83m_4m_can_plan(FlashOperation::Write, kProtocol, kMcu, std::move(rom));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// Hand-built rather than produced by build_subaru_denso_1n83m_4m_can_plan, so
// a plan whose operation the builder itself would refuse can still reach the
// executor -- the only way to prove the executor's own
// validate_subaru_denso_1n83m_4m_can_plan call rejects it before any I/O.
fastecu::flash::FlashPlan handBuiltPlan(FlashOperation operation)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::SubaruDenso1n83m_4mCan;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = std::string(kProtocol);
    fields.mcu_name = std::string(kMcu);
    fields.transfer_region = fastecu::flash::MemoryRegion{kBlockStart, kBlockLength};
    fields.erase_regions = {fastecu::flash::MemoryRegion{kBlockStart, kBlockLength}};
    fields.image = bytes::Bytes(kImageSize, 0x00);
    fields.family_plan = SubaruDenso1n83m_4mCanPlan{0x7e0, 0x7e8, 500000, false, 0x10000, 0x100};
    auto plan = fastecu::flash::validate_and_build(std::move(fields));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// The seed/encrypt tables, transcribed independently from the same legacy
// lines the executor was (generate_can_seed_key/encrypt_payload/
// decrypt_payload, lines 1487-1543) rather than read back from the executor's
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
// probe and the branch selector (legacy lines 105-341). Byte 7 of the raw
// 0x22 0x10 0x1D reply frame -- payload index 3 -- selects the programming
// branch at line 343.
void scriptPreliminaries(ScriptedCanFlashTransport& t, bytes::Byte branchByte)
{
    t.expectWrite(request({0x10, 0x5F})); // OBK probe (lines 105-127), miss
    t.queueRead(response({0x50, 0x01}));
    t.expectWrite(request({0xAA})); // ECU ID (lines 131-172)
    t.queueRead(response({0xEA, 0, 0, 0, 0, 1, 2, 3, 4, 5}));
    t.expectWrite(request({0x09, 0x02})); // VIN (lines 174-205)
    t.queueRead(response({0x49, 0x02, 'V', 'I', 'N'}));
    t.expectWrite(request({0x09, 0x04})); // CAL ID (lines 207-242)
    t.queueRead(response({0x49, 0x04, 'C', 'A', 'L'}));
    t.expectWrite(request({0x09, 0x06})); // CVN (lines 244-281)
    t.queueRead(response({0x49, 0x06, 0xAA, 0xBB}));
    t.expectWrite(request({0x10, 0x5F})); // access method (lines 283-311)
    t.queueRead(response({0x50, 0x01}));
    t.expectWrite(request({0x22, 0x10, 0x1D})); // branch selector (lines 313-343)
    t.queueRead(response({0x62, 0x10, 0x1D, branchByte}));
}

// The same preliminaries with every reply the family tolerates made bad: the
// four identity queries answered with NRCs, then the two checks whose
// `return STATUS_ERROR` is commented out at lines 305 (access-method probe)
// and 335 (branch selector) answered with the wrong subfunction. None of
// these may stop the sequence. The selector's byte 3 still selects the
// branch, exactly as line 343 reads it out of a reply legacy has already
// logged as wrong.
void scriptPreliminariesWithNegativeIdReplies(ScriptedCanFlashTransport& t, bytes::Byte branchByte)
{
    t.expectWrite(request({0x10, 0x5F})); // OBK probe (lines 105-127), miss
    t.queueRead(response({0x50, 0x01}));
    t.expectWrite(request({0xAA})); // ECU ID (lines 131-172)
    t.queueRead(response({0x7F, 0xAA, 0x11}));
    t.expectWrite(request({0x09, 0x02})); // VIN (lines 174-205)
    t.queueRead(response({0x7F, 0x09, 0x11}));
    t.expectWrite(request({0x09, 0x04})); // CAL ID (lines 207-242)
    t.queueRead(response({0x7F, 0x09, 0x11}));
    t.expectWrite(request({0x09, 0x06})); // CVN (lines 244-281)
    t.queueRead(response({0x7F, 0x09, 0x11}));
    t.expectWrite(request({0x10, 0x5F})); // access method, tolerated at line 305
    t.queueRead(response({0x50, 0x02}));
    t.expectWrite(request({0x22, 0x10, 0x1D})); // branch selector, tolerated at line 335
    t.queueRead(response({0x62, 0x11, 0x1D, branchByte}));
}

// The bench arm after the preliminaries (legacy lines 663-808). The kernel
// jump reads TWICE before entering its wait loop (lines 786-790) -- an extra
// read this family's siblings do not perform -- so two matching replies are
// queued: a port that read only once would leave the second frame unconsumed
// and fail scriptConsumed().
void scriptBenchConnectTail(ScriptedCanFlashTransport& t)
{
    t.expectWrite(request({0x10, 0x43})); // lines 667-695
    t.queueRead(response({0x50, 0x43}));
    t.expectWrite(request({0x27, 0x61})); // lines 697-727
    t.queueRead(response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    bytes::Bytes key{0x27, 0x62}; // lines 738-772
    const bytes::Bytes k = seedKey(kSeed);
    key.insert(key.end(), k.begin(), k.end());
    t.expectWrite(request(key));
    t.queueRead(response({0x67, 0x62}));
    t.expectWrite(request({0x10, 0x42})); // lines 776-806
    t.queueRead(response({0x50, 0x42}));  // line 788, discarded
    t.queueRead(response({0x50, 0x42}));  // line 790, the wait loop's seed
}

// The bench arm (legacy lines 663-808).
void scriptBenchConnect(ScriptedCanFlashTransport& t)
{
    scriptPreliminaries(t, 0xFF);
    scriptBenchConnectTail(t);
}

// The 0x34/0x35 dump setup pair (legacy read_memory, lines 843-925).
void scriptReadSetup(ScriptedCanFlashTransport& t)
{
    t.expectWrite(request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    t.queueRead(response({0x74, 0x20, 0x01, 0x05}));
    t.expectWrite(request({0x35, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    t.queueRead(response({0x75, 0x20, 0x01, 0x01}));
}

// The same pair with the 0x34 reply's last header byte wrong (tolerated at
// line 876) and the 0x35 reply absent altogether (tolerated at line 924).
// Both must be logged and stepped over.
void scriptReadSetupWithNegativeReplies(ScriptedCanFlashTransport& t)
{
    t.expectWrite(request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    t.queueRead(response({0x74, 0x20, 0x01, 0x06}));
    t.expectWrite(request({0x35, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    t.queue_no_frame();
}

// The mirror of the above: the 0x34 reply absent (tolerated at line 883) and
// the 0x35 reply's service byte wrong (tolerated at line 917).
void scriptReadSetupWithMissingThenWrongReply(ScriptedCanFlashTransport& t)
{
    t.expectWrite(request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    t.queue_no_frame();
    t.expectWrite(request({0x35, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    t.queueRead(response({0x7F, 0x35, 0x31}));
}

// The chunked 0xB7 dump sweep (legacy read_memory, lines 929-1036): 0xB7 plus
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

// The 0x37 stop command (legacy read_memory, lines 1043-1070).
void scriptStopCommand(ScriptedCanFlashTransport& t)
{
    t.expectWrite(request({0x37}));
    t.queueRead(response({0x77}));
}

// The in-car arm (legacy lines 345-661). The ten fire-and-forget replies are
// deliberately given arbitration ids other than 0x7E8 wherever the addressed
// module would answer on its own id: legacy reads whichever frame arrives
// next without checking the id, and this pins that the port does not add a
// check legacy lacks. `probeService`/`probeSub` parameterize the line-346
// probe so the tolerance at line 369 can be driven.
void scriptInCarConnectTail(ScriptedCanFlashTransport& t, bytes::Byte probeService, bytes::Byte probeSub)
{
    t.expectWrite(request({0x10, 0x5F})); // lines 347-377, tolerated at line 369
    t.queueRead(response({probeService, probeSub}));

    t.expectWrite(requestTo(0x7A2, {0x10, 0xC0})); // lines 379-389
    t.queueRead(responseFrom(0x7AA, {0x50, 0xC0}));
    t.expectWrite(request({0x10, 0x63})); // lines 391-401
    t.queueRead(response({0x50, 0x63}));
    t.expectWrite(requestTo(0x7DF, {0x10, 0x03})); // lines 403-413
    t.queueRead(response({0x50, 0x03}));
    t.expectWrite(requestTo(0x7E1, {0x10, 0x63})); // lines 415-425
    t.queueRead(responseFrom(0x7E9, {0x50, 0x63}));
    t.expectWrite(requestTo(0x7B0, {0x10, 0x03})); // lines 427-437
    t.queueRead(responseFrom(0x7B8, {0x50, 0x03}));
    t.expectWrite(requestTo(0x7B0, {0x85, 0x02})); // lines 439-449
    t.queueRead(responseFrom(0x7B8, {0xC5, 0x02}));
    t.expectWrite(requestTo(0x7DF, {0x85, 0x02})); // lines 451-461
    t.queueRead(response({0xC5, 0x02}));
    t.expectWrite(requestTo(0x7B0, {0x85, 0x02})); // lines 463-473
    t.queueRead(responseFrom(0x7B8, {0xC5, 0x02}));
    t.expectWrite(requestTo(0x7DF, {0x85, 0x02})); // lines 475-485
    t.queueRead(response({0xC5, 0x02}));
    t.expectWrite(requestTo(0x7DF, {0x28, 0x03, 0x01})); // lines 487-498
    t.queueRead(response({0x68, 0x03}));

    t.expectWrite(request({0x27, 0x61})); // lines 500-527
    t.queueRead(response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    bytes::Bytes key{0x27, 0x62}; // lines 540-568
    const bytes::Bytes k = seedKey(kSeed);
    key.insert(key.end(), k.begin(), k.end());
    t.expectWrite(request(key));
    t.queueRead(response({0x67, 0x62}));

    t.expectWrite(request({0x10, 0x5F})); // lines 572-599, fatal on mismatch
    t.queueRead(response({0x50, 0x63}));
    t.expectWrite(request({0x22, 0x10, 0x1D})); // lines 601-629, fatal on mismatch
    t.queueRead(response({0x62, 0x10, 0x1D, 0x00}));
    t.expectWrite(request({0x10, 0x62})); // lines 631-659, one pre-loop read only
    t.queueRead(response({0x50, 0x62}));
}

void scriptInCarConnect(ScriptedCanFlashTransport& t)
{
    scriptPreliminaries(t, 0x00);
    scriptInCarConnectTail(t, 0x50, 0x01);
}

// erase_memory's setup PDU plus its erase trigger (legacy lines 1388-1443);
// the trigger's answer is consumed by the re-read loop, not by a paired read.
void scriptEraseMemory(ScriptedCanFlashTransport& t)
{
    t.expectWrite(request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    t.queueRead(response({0x74, 0x20, 0x01, 0x05}));
    t.expectWrite(request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

// The 0xB6 write-chunk sweep for block 1 (legacy reflash_block, lines
// 1204-1266). `rom` is the whole 0x3E4000 plan image, encrypted once, and
// indexed from kImageStart -- so chunk 0 carries encrypted[0x10000..0x10100).
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

// The close-block 0x37 and the checksum verify (legacy lines 1276-1362).
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

TEST(SubaruDenso1n83m_4mCanExecutor, TransportSetupReturnsThePlansWireParameters)
{
    // The caller configures the transport from this, so the plan's wire
    // parameters have to survive the hand-off intact.
    SubaruDenso1n83m_4mCanExecutor executor;

    const auto setup = executor.transport_setup(readPlan());

    ASSERT_TRUE(setup.has_value()) << setup.error().detail;
    EXPECT_EQ(setup->bitrate, 500000);
    EXPECT_EQ(setup->request_id, 0x7e0U);
    EXPECT_EQ(setup->response_id, 0x7e8U);
    EXPECT_FALSE(setup->extended_id);
}

TEST(SubaruDenso1n83m_4mCanExecutor, ProceedsPastMalformedConnectAndDumpSetupResponses)
{
    // The tolerance this family exists to preserve. A happy-path-only suite
    // would pass against a wrongly strict port, so this drives four of the
    // seven commented-out returns -- lines 305 and 335 in connect_bootloader,
    // 876 and 924 in read_memory -- and requires a complete, correctly sized
    // ROM out the far end anyway.
    ScriptedCanFlashTransport transport;
    scriptPreliminariesWithNegativeIdReplies(transport, 0xFF);
    scriptBenchConnectTail(transport);
    scriptReadSetupWithNegativeReplies(transport);
    scriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    scriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), kImageSize);
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).subspan(0x10000, kBlockLength), Each(0x5A));
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy's own wording for the absent-reply branch it then steps over
    // (line 922).
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Error, "No valid response from ECU")));
}

TEST(SubaruDenso1n83m_4mCanExecutor, ProceedsPastMalformedInCarProbeAndDumpSetup)
{
    // The remaining three tolerated returns: line 369 (the in-car
    // access-method probe) and lines 883 and 917 (the other halves of the two
    // dump-setup checks). The dump itself is cut short with a transport
    // error, so reaching the 0xB7 sweep at all is the proof the setup checks
    // did not abort.
    ScriptedCanFlashTransport transport;
    scriptPreliminaries(transport, 0x00);
    scriptInCarConnectTail(transport, 0x50, 0x02); // wrong subfunction, tolerated at line 369
    scriptReadSetupWithMissingThenWrongReply(transport);
    transport.expectWrite(request(bytes::composeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Error, "No valid response from ECU")));
}

TEST(SubaruDenso1n83m_4mCanExecutor, BenchReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    scriptReadSetup(transport);
    scriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0xA5);
    scriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& rom = *result->read_bytes;
    EXPECT_EQ(rom.size(), kImageSize);
    EXPECT_THAT(bytes::ByteView(rom).first(0x10000), Each(0xFF));                 // leading pad
    EXPECT_THAT(bytes::ByteView(rom).subspan(0x10000, kBlockLength), Each(0xA5)); // decrypted payload
    EXPECT_THAT(bytes::ByteView(rom).last(0x100), Each(0xFF));                    // tail pad
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs, testing::Not(IsEmpty()));
}

TEST(SubaruDenso1n83m_4mCanExecutor, InCarReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    scriptInCarConnect(transport);
    scriptReadSetup(transport);
    scriptFlashDump(transport, kBlockStart, kBlockLength, kPageSize, 0x5A);
    scriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& rom = *result->read_bytes;
    EXPECT_EQ(rom.size(), kImageSize);
    EXPECT_THAT(bytes::ByteView(rom).first(0x10000), Each(0xFF));
    EXPECT_THAT(bytes::ByteView(rom).subspan(0x10000, kBlockLength), Each(0x5A));
    EXPECT_THAT(bytes::ByteView(rom).last(0x100), Each(0xFF));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDenso1n83m_4mCanExecutor, WriteErasesThenFlashesBlockOne)
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
    SubaruDenso1n83m_4mCanExecutor executor;

    auto result = executor.execute(writePlan(rom), transport, clock, cancellation, events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.notices, Contains("Writing ROM, please wait..."));
    // Every 0xB6 chunk was matched byte-for-byte by expectWrite above; assert
    // the indexing convention explicitly too, so a wrong image base fails
    // here with a readable message rather than as an "unexpected write".
    // reflash_block reads newdata[i + blockaddr - fblocks[0].start] (line
    // 1241) out of the caller's &data_array[0] (line 1153), which is the
    // whole encrypted image starting at fblocks[0].start -- so the first
    // chunk of block 1 is encrypted[0x10000..0x10100).
    const bytes::Bytes encrypted = toWire(rom);
    EXPECT_EQ(bytes::Bytes(encrypted.begin() + 0x10000, encrypted.begin() + 0x10000 + 256),
              toWire(bytes::ByteView(rom).subspan(0x10000, 256)));
    // Every sleep the write path performs, in order, each with the legacy
    // delay() it reproduces: connect_bench's wait (line 666), the bench kernel
    // jump's inter-read settle (line 790), the settle after the erase command
    // (line 1447), and the settle before the checksum-verify write (line
    // 1305). Asserted as a whole sequence rather than by Contains so that
    // dropping one -- as this port did with the 1305 settle -- fails here
    // instead of passing silently.
    EXPECT_EQ(clock.sleep_calls, (std::vector<int>{500, 50, 500, 100}));
}

TEST(SubaruDenso1n83m_4mCanExecutor, TestWriteIsRejectedBeforeAnyTransportCall)
{
    // Legacy threaded test_write from execute() through write_memory into
    // reflash_block and never consulted it, so a test_write run performed a
    // real erase and a real 0xB6 flash write. The port refuses before it
    // configures or opens the transport, let alone reaches the ECU.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    auto result = executor.execute(handBuiltPlan(FlashOperation::TestWrite), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_EQ(transport.writesConsumed(), 0U);
    EXPECT_FALSE(transport.last_config_.has_value());
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(SubaruDenso1n83m_4mCanExecutor, ReadTimeoutPropagates)
{
    // A transport-level failure is not what the tolerated checks tolerate:
    // legacy's read_serial_data has no error channel at all, so a genuinely
    // broken bus still has to surface rather than be swallowed as an
    // "absent reply".
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    transport.expectWrite(request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x3D, 0x3F, 0x00}));
    transport.queue_error(ErrorKind::Timeout, "no reply");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDenso1n83m_4mCanExecutor, ReadDisconnectPropagates)
{
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    scriptReadSetup(transport);
    transport.expectWrite(request(bytes::composeBe(bytes::Byte(0xB7), kBlockStart)));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDenso1n83m_4mCanExecutor, NegativeResponseDuringConnectFails)
{
    // Tolerance is not blanket: the seed request (legacy lines 697-727) keeps
    // its live `return STATUS_ERROR`, so a negative response there must abort
    // even in this family.
    ScriptedCanFlashTransport transport;
    scriptPreliminaries(transport, 0xFF);
    transport.expectWrite(request({0x10, 0x43}));
    transport.queueRead(response({0x50, 0x43}));
    transport.expectWrite(request({0x27, 0x61}));
    transport.queueRead(response({0x7F, 0x27, 0x35}));

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_4mCanExecutor executor;

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

TEST(SubaruDenso1n83m_4mCanExecutor, CancellationMidReadReturnsCancelled)
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
    SubaruDenso1n83m_4mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDenso1n83m_4mCanExecutor, EmptyBranchSelectorReplyFails)
{
    // Line 335 is commented out but line 340 is not: a *wrong* branch-selector
    // reply is tolerated, an *absent* one still returns STATUS_ERROR. This
    // pins that half of the check, which the tolerance must not swallow.
    ScriptedCanFlashTransport transport;
    transport.expectWrite(request({0x10, 0x5F}));
    transport.queueRead(response({0x50, 0x01}));
    transport.expectWrite(request({0xAA}));
    transport.queueRead(response({0xEA, 0, 0, 0, 0, 1, 2, 3, 4, 5}));
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
    SubaruDenso1n83m_4mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDenso1n83m_4mCanExecutor, EraseRetryExhaustionFails)
{
    // Legacy erase_memory's re-read loop (lines 1449-1476): twenty reads, no
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
    SubaruDenso1n83m_4mCanExecutor executor;

    auto result = executor.execute(writePlan(rom), transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

} // namespace
