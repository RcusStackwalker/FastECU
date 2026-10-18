#include "src/backend/flash/ecu/denso_beef_can_common.h"

#include <array>
#include <chrono>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{

using namespace std::chrono_literals;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::StrEq;

TEST(DensoBeefCanCommonTest, BeefRequestFramesOpcodeAndPayloadLength)
{
    const bytes::Bytes payload{0x01, 0x02, 0x03};
    const bytes::Bytes framed = BeefRequest(0xB6, payload);

    ASSERT_EQ(framed.size(), 8U);
    EXPECT_EQ(framed[0], 0xBE);
    EXPECT_EQ(framed[1], 0xEF);
    EXPECT_EQ(framed[2], 0x00);
    EXPECT_EQ(framed[3], 0x04);
    EXPECT_EQ(framed[4], 0xB6);
    EXPECT_EQ(framed[5], 0x01);
    EXPECT_EQ(framed[6], 0x02);
    EXPECT_EQ(framed[7], 0x03);
}

TEST(DensoBeefCanCommonTest, BeefRequestWithNoPayloadStillCountsTheOpcode)
{
    const bytes::Bytes framed = BeefRequest(0xA0);

    ASSERT_EQ(framed.size(), 5U);
    EXPECT_EQ(framed[3], 0x01);
    EXPECT_EQ(framed[4], 0xA0);
}

TEST(DensoBeefCanCommonTest, ElapsedMillisecondsReportsAtLeastOne)
{
    const std::chrono::steady_clock::time_point start{};

    EXPECT_EQ(ElapsedMilliseconds(start, start), 1U);
    EXPECT_EQ(ElapsedMilliseconds(start, start + 250ms), 250U);
}

TEST(DensoBeefCanCommonTest, CancelledIfRequestedReportsTheCallerDetail)
{
    struct Ctx
    {
        const ICancellationToken& cancellation;
    };
    FakeCancellationToken token;
    const Ctx ctx{token};

    EXPECT_THAT(CancelledIfRequested(ctx, "cancelled before CAN request"), IsOk());

    token.SetCancelled(true);
    const Status cancelled = CancelledIfRequested(ctx, "cancelled before CAN request");
    EXPECT_THAT(cancelled, IsErrWith(ErrorKind::kCancelled, StrEq("cancelled before CAN request")));
}

TEST(DensoBeefCanCommonTest, ParseBeefBoundsEveryDeclaredField)
{
    const auto parse = [](bytes::Bytes pdu) { return ParseBeef(pdu); };
    EXPECT_THAT(parse({0xBE, 0xEF, 0x00}), IsErrWith(ErrorKind::kBadResponse, StrEq("short BEEF response envelope")));
    EXPECT_THAT(parse({0x12, 0x34, 0x00, 0x01, 0x00}),
                IsErrWith(ErrorKind::kBadResponse, StrEq("wrong BEEF response marker")));
    EXPECT_THAT(parse({0xBE, 0xEF, 0x00, 0x09, 0x00}),
                IsErrWith(ErrorKind::kBadResponse, StrEq("invalid BEEF response length")));
    EXPECT_THAT(parse({0xBE, 0xEF, 0x00, 0x00, 0x00}),
                IsErrWith(ErrorKind::kBadResponse, StrEq("invalid BEEF response length")));
    for (const auto& [pdu, payload_size] : {std::pair{bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0xB6}, 0U},
                                            std::pair{bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0xB6, 0xAA, 0xFF}, 1U}})
    {
        const auto parsed = ParseBeef(pdu);
        ASSERT_THAT(parsed, IsOk());
        EXPECT_EQ(parsed->opcode, 0xB6);
        EXPECT_EQ(parsed->payload.size(), payload_size);
    }
}

// A real petrol write plan stands in for any BEEF family's: CompareBeefBlocks
// reads only its transfer and erase regions.
struct CompareFixture
{
    CompareFixture()
    {
        auto built = BuildSubaruDensoSh7058CanPlan(
            FlashOperation::kWrite, "sub_ecu_denso_sh7058_can", "SH7058", bytes::Bytes(0x100000, bytes::Byte{0}),
            KernelImage{.id = "kernel", .load_address = 0xFFFF3000, .bytes = {0x01, 0x02, 0x03, 0x04}});
        EXPECT_THAT(built, IsOk());
        plan.emplace(std::move(*built));
    }

    std::optional<FlashPlan> plan;
    FakeCancellationToken cancellation;
    RecordingEventSink sink;
    FakeClock fake_clock;
    IEventSink& events{sink};
    IClock& clock{fake_clock};
    bytes::Bytes image = bytes::Bytes(0x100000, bytes::Byte{0});
};

TEST(DensoBeefCanCommonTest, CompareBeefBlocksRejectsAnImageShorterThanTheRegionsAndNamesTheFamily)
{
    CompareFixture fixture;
    const bytes::Bytes short_image(16, bytes::Byte{0});

    const auto result = CompareBeefBlocks(fixture, *fixture.plan, short_image, nullptr, true, "petrol",
                                          [](const MemoryRegion&) -> Result<std::uint32_t> { return 0U; });

    EXPECT_THAT(result, IsErrWith(ErrorKind::kInvalidConfig, StrEq("petrol CRC block is outside the image")));
}

TEST(DensoBeefCanCommonTest, CompareBeefBlocksStopsOnQueryFailureAndOnCancellationDuringTheSettle)
{
    CompareFixture failing;
    EXPECT_THAT(CompareBeefBlocks(failing, *failing.plan, failing.image, nullptr, false, "petrol",
                                  [](const MemoryRegion&) -> Result<std::uint32_t>
                                  { return Fail(ErrorKind::kTimeout, "no CRC reply"); }),
                IsErrWith(ErrorKind::kTimeout, StrEq("no CRC reply")));

    CompareFixture cancelling;
    EXPECT_THAT(CompareBeefBlocks(cancelling, *cancelling.plan, cancelling.image, nullptr, false, "petrol",
                                  [&](const MemoryRegion&) -> Result<std::uint32_t>
                                  {
                                      cancelling.cancellation.SetCancelled(true);
                                      return 0U;
                                  }),
                IsErr(ErrorKind::kCancelled));
}

TEST(DensoBeefCanCommonTest, InitializeBeefFlashSelectsTheModeAndStopsAtTheFirstFailingExchange)
{
    for (const bool test_write : {false, true})
    {
        CompareFixture fixture;
        std::vector<bytes::Byte> opcodes;
        EXPECT_THAT(InitializeBeefFlash(fixture, test_write, test_write,
                                        [&](bytes::Byte opcode, std::size_t) -> Result<bytes::Bytes>
                                        {
                                            opcodes.push_back(opcode);
                                            return bytes::Bytes{0x00, 0x00, 0xAB, 0xCD};
                                        }),
                    IsOk());
        EXPECT_EQ(opcodes, (std::vector<bytes::Byte>{0x05, 0x06, test_write ? bytes::Byte{0x21} : bytes::Byte{0x20}}));
        EXPECT_EQ(fixture.sink.logs[1].second, test_write ? ": 0xABCD" : ": 0xabcd");
    }
    for (const std::size_t failing : {0U, 1U, 2U})
    {
        CompareFixture fixture;
        std::size_t calls = 0;
        EXPECT_THAT(InitializeBeefFlash(fixture, false, false,
                                        [&](bytes::Byte, std::size_t) -> Result<bytes::Bytes>
                                        {
                                            return calls++ == failing ? Fail(ErrorKind::kTimeout, "no reply")
                                                                      : Result<bytes::Bytes>{bytes::Bytes(4)};
                                        }),
                    IsErrWith(ErrorKind::kTimeout, StrEq("no reply")));
        EXPECT_EQ(calls, failing + 1);
    }
}

TEST(DensoBeefCanCommonTest, BeefKernelUploadPayloadNamesTheFamilyInBothSizeErrors)
{
    EXPECT_THAT(BeefKernelUploadPayload({}, 128, "petrol"),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("petrol kernel upload size is invalid")));
    EXPECT_THAT(BeefKernelUploadPayload(bytes::Bytes(0x1000000), 128, "diesel"),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("diesel kernel upload exceeds the 24-bit wire length")));
    const auto small = BeefKernelUploadPayload(bytes::Bytes(5), 8, "petrol");
    ASSERT_THAT(small, IsOk());
    EXPECT_EQ(small->size(), 8U);
}

TEST(DensoBeefCanCommonTest, SendChannelRequestSleepsOnlyForAPositiveDelay)
{
    struct Channel
    {
        Status Send(bytes::ByteView, const ICancellationToken&)
        {
            return {};
        }
    };
    struct Ctx
    {
        Channel channel;
        FakeClock clock;
        FakeCancellationToken cancellation;
    } ctx;
    ctx.clock.SetSleepAdvance(5ms);

    EXPECT_THAT(SendChannelRequest(ctx, bytes::Bytes{0x01}, 0ms), IsOk());
    EXPECT_EQ(ctx.clock.Elapsed(), 0ms);
    EXPECT_THAT(SendChannelRequest(ctx, bytes::Bytes{0x01}, 10ms), IsOk());
    EXPECT_EQ(ctx.clock.Elapsed(), 5ms);
}

TEST(DensoBeefCanCommonTest, UppercaseHexCompactHasNoSeparators)
{
    EXPECT_EQ(UppercaseHexCompact(bytes::Bytes{0x0A, 0xFF, 0x00}), "0AFF00");
}

TEST(DensoBeefCanCommonTest, BeefKernelPayloadReplacesThePaddedLastWordWithTheChecksum)
{
    // Five bytes pad to one eight-byte block, whose last word (holding the
    // kernel's fifth byte) becomes 0x5AA5A55A minus the sum of the others.
    const auto payload = BeefKernelPayload(bytes::Bytes{0x00, 0x00, 0x00, 0x01, 0xAA}, 8);

    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(*payload, EncryptPayload(bytes::Bytes{0x00, 0x00, 0x00, 0x01, 0x5A, 0xA5, 0xA5, 0x59}));
    EXPECT_FALSE(BeefKernelPayload({}, 8).has_value());
}

TEST(DensoBeefCanCommonTest, UploadBeefKernelBlocksEndsWithAnAddressOnlyBlock)
{
    struct Ctx
    {
        const ICancellationToken& cancellation;
    };
    FakeCancellationToken token;
    const Ctx ctx{token};
    std::vector<bytes::Bytes> requests;

    const Status uploaded = UploadBeefKernelBlocks(ctx, bytes::Bytes{0x01, 0x02, 0x03, 0x04}, 0x1000, 2,
                                                   [&requests](bytes::ByteView request) -> Status
                                                   {
                                                       requests.emplace_back(request.begin(), request.end());
                                                       return {};
                                                   });

    EXPECT_THAT(uploaded, IsOk());
    EXPECT_EQ(requests, (std::vector<bytes::Bytes>{{0xB6, 0x00, 0x10, 0x00, 0x01, 0x02},
                                                   {0xB6, 0x00, 0x10, 0x02, 0x03, 0x04},
                                                   {0xB6, 0x00, 0x10, 0x04}}));
}

} // namespace
} // namespace fastecu::flash
