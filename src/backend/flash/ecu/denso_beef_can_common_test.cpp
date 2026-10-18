#include "src/backend/flash/ecu/denso_beef_can_common.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <numeric>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

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

// The context shape the shared helpers expect: cancellation, events and clock.
struct CompareFixture
{
    FakeCancellationToken cancellation;
    RecordingEventSink sink;
    FakeClock fake_clock;
    IEventSink& events{sink};
    IClock& clock{fake_clock};
};

// Three 4-byte regions of an image that starts at ECU address 0x1000.
constexpr std::uint32_t kImageBase = 0x1000;
constexpr std::array<MemoryRegion, 3> kRegions{{{0x1000, 4}, {0x1004, 4}, {0x1008, 4}}};

TEST(DensoBeefCanCommonTest, CompareBeefBlocksChecksEachRegionAgainstItsOwnSliceOfTheImage)
{
    CompareFixture fixture;
    bytes::Bytes image(12);
    std::iota(image.begin(), image.end(), bytes::Byte{1});
    std::size_t queried = 0;

    const auto result =
        CompareBeefBlocks(fixture, kRegions, kImageBase, image, nullptr, false, "petrol",
                          [&](const MemoryRegion& block) -> Result<std::uint32_t>
                          {
                              const std::uint32_t crc = fastecu::checksum::Crc32(
                                  bytes::ByteView{image}.subspan(block.start - kImageBase, block.length));
                              return queried++ == 1 ? crc ^ 1U : crc;
                          });

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(queried, kRegions.size());
    EXPECT_EQ(result->changed_count, 1U);
    EXPECT_EQ(result->modified, (std::vector<bool>{false, true, false}));
    EXPECT_EQ(fixture.fake_clock.Elapsed(), 5ms * queried);
    EXPECT_EQ(fixture.sink.logs.at(fixture.sink.logs.size() - 2).second, "1, ");
}

TEST(DensoBeefCanCommonTest, CompareBeefBlocksRejectsRegionsOutsideTheImageAndNamesTheFamily)
{
    const auto never_asked = [](const MemoryRegion&) -> Result<std::uint32_t> { return 0U; };
    const bytes::Bytes image(12);
    CompareFixture fixture;

    EXPECT_THAT(CompareBeefBlocks(fixture, kRegions, kImageBase, bytes::ByteView{image}.first(11), nullptr, true,
                                  "petrol", never_asked),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("petrol CRC block is outside the image")));
    EXPECT_THAT(CompareBeefBlocks(fixture, kRegions, kImageBase + 1, image, nullptr, true, "petrol", never_asked),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("petrol CRC block is outside the image")));
    EXPECT_THAT(CompareBeefBlocks(fixture, kRegions, kImageBase, image, nullptr, true, "petrol", never_asked), IsOk());
}

TEST(DensoBeefCanCommonTest, CompareBeefBlocksStopsOnQueryFailureAndOnCancellationDuringTheSettle)
{
    const bytes::Bytes image(12);
    CompareFixture failing;
    EXPECT_THAT(CompareBeefBlocks(failing, kRegions, kImageBase, image, nullptr, false, "petrol",
                                  [](const MemoryRegion&) -> Result<std::uint32_t>
                                  { return Fail(ErrorKind::kTimeout, "no CRC reply"); }),
                IsErrWith(ErrorKind::kTimeout, StrEq("no CRC reply")));

    CompareFixture cancelling;
    EXPECT_THAT(CompareBeefBlocks(cancelling, kRegions, kImageBase, image, nullptr, false, "petrol",
                                  [&](const MemoryRegion&) -> Result<std::uint32_t>
                                  {
                                      cancelling.cancellation.SetCancelled(true);
                                      return 0U;
                                  }),
                IsErr(ErrorKind::kCancelled));
}

// A scripted channel and raw transport behind the context shape the shared
// exchange helpers expect.
struct ExchangeFixture
{
    struct Channel
    {
        Status Send(bytes::ByteView, const ICancellationToken&)
        {
            return send_result;
        }
        Result<std::optional<bytes::Bytes>> Receive(std::chrono::milliseconds, const ICancellationToken&)
        {
            return reply;
        }
        Status send_result;
        Result<std::optional<bytes::Bytes>> reply{std::nullopt};
    };
    struct Transport
    {
        Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds, const ICancellationToken&)
        {
            return stale;
        }
        Result<std::optional<bytes::Bytes>> stale{std::nullopt};
    };

    Channel channel;
    Transport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
};

TEST(DensoBeefCanCommonTest, BeefExchangeReturnsThePayloadOfAnOpcodeOrSizeCheckedReply)
{
    ExchangeFixture fixture;
    const auto exchange = [&](bytes::Bytes reply, std::size_t minimum)
    {
        fixture.channel.reply = std::move(reply);
        return BeefExchange(fixture, 0x05, {}, minimum, 10ms);
    };
    const bytes::Bytes good{0xBE, 0xEF, 0x00, 0x03, 0x45, 0xAA, 0xBB};

    EXPECT_THAT(exchange(good, 2), IsOk());
    EXPECT_THAT(exchange(good, 3), IsErrWith(ErrorKind::kBadResponse, StrEq("short BEEF response payload")));
    EXPECT_THAT(exchange({0xBE, 0xEF, 0x00, 0x01, 0x46}, 0),
                IsErrWith(ErrorKind::kBadResponse, StrEq("unexpected BEEF response opcode 0x46, expected 0x45")));
    EXPECT_THAT(exchange({0xBE}, 0), IsErrWith(ErrorKind::kBadResponse, StrEq("short BEEF response envelope")));

    fixture.channel.reply = std::nullopt;
    EXPECT_THAT(BeefExchange(fixture, 0x05, {}, 0, 10ms),
                IsErrWith(ErrorKind::kTimeout, StrEq("no CAN response within the read timeout")));
    fixture.channel.reply = Fail(ErrorKind::kDisconnected, "gone");
    EXPECT_THAT(BeefExchange(fixture, 0x05, {}, 0, 10ms), IsErr(ErrorKind::kDisconnected));
}

TEST(DensoBeefCanCommonTest, DiscardB6ReplyIgnoresStaleContentButNotCancellationOrDisconnection)
{
    ExchangeFixture fixture;
    const bytes::Bytes pdu{0xB6};

    EXPECT_THAT(DiscardB6Reply(fixture, pdu, 10ms), IsOk());
    fixture.transport.stale = Fail(ErrorKind::kBadResponse, "noise");
    EXPECT_THAT(DiscardB6Reply(fixture, pdu, 10ms), IsOk());
    fixture.transport.stale = Fail(ErrorKind::kDisconnected, "gone");
    EXPECT_THAT(DiscardB6Reply(fixture, pdu, 10ms), IsErr(ErrorKind::kDisconnected));
    fixture.transport.stale = std::nullopt;
    fixture.channel.send_result = Fail(ErrorKind::kTimeout, "no ack");
    EXPECT_THAT(DiscardB6Reply(fixture, pdu, 10ms), IsErr(ErrorKind::kTimeout));
    fixture.cancellation.SetCancelled(true);
    EXPECT_THAT(DiscardB6Reply(fixture, pdu, 10ms), IsErr(ErrorKind::kCancelled));
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
