#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/denso_beef_kline_common.h"

#include <array>
#include <gtest/gtest.h>
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/mock_clock.h"

namespace fastecu::flash
{
namespace
{
TEST(DensoBeefKlineCommonTest, FramesLiteralRequests)
{
    EXPECT_EQ(DensoBeefKlineFrame(0x01), (bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x01, 0xAF}));
    EXPECT_EQ(DensoBeefKlineFrame(0x22, bytes::Bytes{0x01, 0x02}),
              (bytes::Bytes{0xBE, 0xEF, 0x00, 0x03, 0x22, 0x01, 0x02, 0xD5}));
}

TEST(DensoBeefKlineCommonTest, LengthConversionWrapsWithoutTruncatingPayload)
{
    const auto frame = DensoBeefKlineFrame(0x22, bytes::Bytes(65535, 0));
    ASSERT_EQ(frame.size(), 65541U);
    EXPECT_EQ(frame[2], 0);
    EXPECT_EQ(frame[3], 0);
}

TEST(DensoBeefKlineCommonTest, AckRequiresOnlyMarkerMinimumSizeAndOpcode)
{
    EXPECT_TRUE(DensoBeefKlineResponseOk(bytes::Bytes{0xBE, 0xEF, 0xFF, 0xFF, 0x41, 0}, 0x41));
    EXPECT_FALSE(DensoBeefKlineResponseOk(bytes::Bytes{0xBE, 0xEF, 0, 1, 0x41}, 0x41));
    EXPECT_FALSE(DensoBeefKlineResponseOk(bytes::Bytes{0xBE, 0xEE, 0, 1, 0x41, 0}, 0x41));
    EXPECT_FALSE(DensoBeefKlineResponseOk(bytes::Bytes{0xBE, 0xEF, 0, 1, 0x42, 0}, 0x41));
}

TEST(DensoBeefKlineCommonTest, EncodesLiteralFirstAndLastChunkAddresses)
{
    EXPECT_EQ(DensoBeefKlineWritePayload(0x00123400, bytes::Bytes{0xA5, 0x5A}),
              (bytes::Bytes{0x00, 0x12, 0x34, 0x00, 0xA5, 0x5A}));
    EXPECT_EQ(DensoBeefKlineWritePayload(0x0012FF00, bytes::Bytes{0xFE, 0xDC}),
              (bytes::Bytes{0x00, 0x12, 0xFF, 0x00, 0xFE, 0xDC}));
}

TEST(DensoBeefKlineCommonTest, ComparesOrderedAddressedSlicesAndStopsOnFailureOrCancellation)
{
    // Independent reflected bit calculation with polynomial 0x5AA5A55A,
    // initial/final XOR 0xFFFFFFFF, for "123456789" and the empty slice.
    const bytes::Bytes image{0xAA, '1', '2', '3', '4', '5', '6', '7', '8', '9', 0xBB};
    const std::array blocks{MemoryRegion{1, 9}, MemoryRegion{11, 0}, MemoryRegion{1, 9}};
    for (int scenario = 0; scenario < 4; ++scenario)
    {
        SCOPED_TRACE(scenario);
        FakeCancellationToken cancellation;
        std::vector<bool> modified(3, false);
        unsigned calls = 0;
        const auto result = DensoBeefKlineCompareBlocks(blocks, image, modified, cancellation,
                                                        [&](const MemoryRegion& block) -> Result<std::uint32_t>
                                                        {
                                                            EXPECT_EQ(block.start, blocks[calls].start);
                                                            EXPECT_EQ(block.length, blocks[calls].length);
                                                            ++calls;
                                                            if (scenario == 2 && calls == 2)
                                                            {
                                                                return Fail(ErrorKind::kDisconnected, "CRC failed");
                                                            }
                                                            if (scenario == 3 && calls == 1)
                                                            {
                                                                cancellation.SetCancelled(true);
                                                            }
                                                            return calls == 2 ? (scenario == 1 ? 1U : 0U) : 0x80C032ABU;
                                                        });
        EXPECT_EQ(calls, scenario == 2 ? 2U : scenario == 3 ? 1U : 3U);
        EXPECT_EQ(modified, (std::vector<bool>{false, scenario == 1, false}));
        if (scenario < 2)
        {
            ASSERT_THAT(result, fastecu::testing::IsOk());
            EXPECT_EQ(*result, scenario == 1 ? 1U : 0U);
        }
        else
        {
            ASSERT_THAT(result,
                        fastecu::testing::IsErr(scenario == 2 ? ErrorKind::kDisconnected : ErrorKind::kCancelled));
            EXPECT_EQ(result.error().detail, scenario == 2 ? "CRC failed" : "cancelled during ROM compare");
        }
    }
}

TEST(DensoBeefKlineCommonTest, ProgrammingLinePreservesCancellationAndErrorPrecedence)
{
    for (int scenario = 0; scenario < 4; ++scenario)
    {
        FakeCancellationToken cancellation;
        if (scenario < 2)
        {
            cancellation.CancelOnCheck(static_cast<std::size_t>(scenario) + 1);
        }
        class Transport final : public ScriptedKlineFlashTransport
        {
          public:
            FakeCancellationToken *cancellation = nullptr;
            bool fail = false;
            unsigned calls = 0;
            Status EnableProgrammingVoltageLine() override
            {
                ++calls;
                if (fail)
                {
                    cancellation->SetCancelled(true);
                    return Fail(ErrorKind::kDisconnected, "line failed");
                }
                return {};
            }
        } transport;
        transport.cancellation = &cancellation;
        transport.fail = scenario == 2;
        const auto result = DensoBeefKlineEnableProgrammingVoltage(transport, cancellation);
        EXPECT_EQ(transport.calls, scenario == 0 ? 0U : 1U);
        if (scenario == 3)
        {
            EXPECT_THAT(result, fastecu::testing::IsOk());
        }
        else
        {
            ASSERT_THAT(result,
                        fastecu::testing::IsErr(scenario == 2 ? ErrorKind::kDisconnected : ErrorKind::kCancelled));
            EXPECT_EQ(result.error().detail, scenario == 0   ? "cancelled before programming line state"
                                             : scenario == 1 ? "cancelled after programming line state"
                                                             : "line failed");
        }
    }
}

using namespace std::chrono_literals;

class ExchangeTraceTransport final : public ScriptedKlineFlashTransport
{
  public:
    explicit ExchangeTraceTransport(FakeCancellationToken& cancellation) : cancellation_(cancellation)
    {
    }
    Result<std::size_t> Write(bytes::ByteView request) override
    {
        trace.push_back("write");
        EXPECT_EQ(bytes::Bytes(request.begin(), request.end()), (bytes::Bytes{0xBE, 0xEF, 0, 1, 1, 0xAF}));
        if (cancel_write)
        {
            cancellation_.SetCancelled(true);
        }
        if (write_error)
        {
            return Fail(ErrorKind::kDisconnected, "write failed");
        }
        return short_write ? request.size() - 1 : request.size();
    }
    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken&) override
    {
        trace.push_back("read");
        EXPECT_EQ(timeout, 1234ms);
        if (cancel_read)
        {
            cancellation_.SetCancelled(true);
        }
        if (read_error)
        {
            return Fail(ErrorKind::kDisconnected, "read failed");
        }
        return empty ? OptionalBytes{bytes::Bytes{}} : OptionalBytes{};
    }
    std::vector<std::string> trace;
    bool cancel_write = false;
    bool cancel_read = false;
    bool write_error = false;
    bool read_error = false;
    bool short_write = false;
    bool empty = false;

  private:
    FakeCancellationToken& cancellation_;
};

TEST(DensoBeefKlineCommonTest, PreservesEveryCancellationCheckpoint)
{
    const std::array details{"cancelled before write", "cancelled after write", "cancelled before read",
                             "cancelled after read"};
    for (std::size_t checkpoint = 1; checkpoint <= 4; ++checkpoint)
    {
        SCOPED_TRACE(checkpoint);
        FakeCancellationToken cancellation;
        cancellation.CancelOnCheck(checkpoint);
        ExchangeTraceTransport transport(cancellation);
        const auto result = DensoBeefKlineExchangeOptional(transport, nullptr, cancellation,
                                                           bytes::Bytes{0xBE, 0xEF, 0, 1, 1, 0xAF}, 200ms, 1234ms);
        ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_EQ(result.error().detail, details[checkpoint - 1]);
        EXPECT_EQ(cancellation.CheckCount(), checkpoint);
        std::vector<std::string> expected;
        if (checkpoint >= 2)
        {
            expected.push_back("write");
        }
        if (checkpoint >= 4)
        {
            expected.push_back("read");
        }
        EXPECT_EQ(transport.trace, expected);
    }
}

TEST(DensoBeefKlineCommonTest, TransportFailurePrecedesConcurrentCancellation)
{
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        SCOPED_TRACE(scenario);
        FakeCancellationToken cancellation;
        ExchangeTraceTransport transport(cancellation);
        transport.cancel_write = scenario != 2;
        transport.cancel_read = scenario == 2;
        transport.write_error = scenario == 0;
        transport.short_write = scenario == 1;
        transport.read_error = scenario == 2;
        const auto result = DensoBeefKlineExchangeOptional(transport, nullptr, cancellation,
                                                           bytes::Bytes{0xBE, 0xEF, 0, 1, 1, 0xAF}, 0ms, 1234ms);
        ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kDisconnected));
        EXPECT_EQ(result.error().kind, ErrorKind::kDisconnected);
        EXPECT_EQ(result.error().detail, scenario == 0   ? "write failed"
                                         : scenario == 1 ? "short K-Line write"
                                                         : "read failed");
        EXPECT_EQ(transport.trace,
                  (scenario == 2 ? std::vector<std::string>{"write", "read"} : std::vector<std::string>{"write"}));
        EXPECT_EQ(cancellation.CheckCount(), scenario == 2 ? 3U : 1U);
    }
}

TEST(DensoBeefKlineCommonTest, SettleAndOptionalPresenceRemainDistinct)
{
    for (int scenario = 0; scenario < 5; ++scenario)
    {
        SCOPED_TRACE(scenario);
        FakeCancellationToken cancellation;
        ExchangeTraceTransport transport(cancellation);
        MockClock clock;
        transport.empty = scenario == 1;
        EXPECT_CALL(clock, Sleep).Times(0);
        if (scenario < 3)
        {
            EXPECT_CALL(clock, Sleep(200ms, ::testing::_))
                .WillOnce(
                    [&](auto, const auto&) -> Status
                    {
                        transport.trace.push_back("sleep");
                        if (scenario == 2)
                        {
                            return Fail(ErrorKind::kDisconnected, "sleep failed");
                        }
                        return {};
                    });
        }
        const auto result = DensoBeefKlineExchangeOptional(transport, scenario == 3 ? nullptr : &clock, cancellation,
                                                           bytes::Bytes{0xBE, 0xEF, 0, 1, 1, 0xAF},
                                                           scenario == 4 ? 0ms : 200ms, 1234ms);
        if (scenario == 2)
        {
            ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kDisconnected));
            EXPECT_EQ(result.error().detail, "sleep failed");
            EXPECT_EQ(result.error().kind, ErrorKind::kDisconnected);
            EXPECT_EQ(transport.trace, (std::vector<std::string>{"write", "sleep"}));
            EXPECT_EQ(cancellation.CheckCount(), 2U);
        }
        else
        {
            ASSERT_THAT(result, fastecu::testing::IsOk());
            EXPECT_EQ(result->has_value(), scenario == 1);
            if (scenario == 1)
            {
                EXPECT_TRUE((**result).empty());
            }
            EXPECT_EQ(transport.trace, (scenario < 3 ? std::vector<std::string>{"write", "sleep", "read"}
                                                     : std::vector<std::string>{"write", "read"}));
            EXPECT_EQ(cancellation.CheckCount(), 4U);
        }
    }
}
TEST(DensoBeefKlineCommonTest, CheckAckNamesThePhaseAndHonoursTheMinimumSize)
{
    const bytes::Bytes ack{0xBE, 0xEF, 0x00, 0x02, 0x62, 0x00, 0x00};
    EXPECT_THAT(DensoBeefKlineCheckAck(ack, 0x22, "write"), fastecu::testing::IsOk());
    EXPECT_THAT(
        DensoBeefKlineCheckAck(ack, 0x22, "write", 7),
        fastecu::testing::IsErrWith(ErrorKind::kBadResponse, ::testing::StrEq("Wrong response from ECU during write")));
    EXPECT_THAT(DensoBeefKlineCheckAck(ack, 0x23, "commit"), fastecu::testing::IsErr(ErrorKind::kBadResponse));
}

TEST(DensoBeefKlineCommonTest, ExchangeAckTimesOutWithoutAFrame)
{
    FakeCancellationToken cancellation;
    ExchangeTraceTransport transport(cancellation);
    const auto result = DensoBeefKlineExchangeAck(transport, nullptr, cancellation,
                                                  bytes::Bytes{0xBE, 0xEF, 0, 1, 1, 0xAF}, 0ms, 1234ms, 0x01, "erase");
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout,
                                                    ::testing::StrEq("no response from ECU during erase")));
}

TEST(DensoBeefKlineCommonTest, GuardedChecksCancellationAroundTheAction)
{
    for (std::size_t checkpoint = 1; checkpoint <= 2; ++checkpoint)
    {
        SCOPED_TRACE(checkpoint);
        FakeCancellationToken cancellation;
        cancellation.CancelOnCheck(checkpoint);
        int calls = 0;
        const Status result = DensoBeefKlineGuarded(cancellation, "LEC pulse",
                                                    [&calls]() -> Status
                                                    {
                                                        ++calls;
                                                        return {};
                                                    });
        EXPECT_THAT(result,
                    fastecu::testing::IsErrWith(ErrorKind::kCancelled,
                                                ::testing::StrEq(checkpoint == 1 ? "cancelled before LEC pulse"
                                                                                 : "cancelled after LEC pulse")));
        EXPECT_EQ(calls, checkpoint == 1 ? 0 : 1);
    }
}

} // namespace
} // namespace fastecu::flash
