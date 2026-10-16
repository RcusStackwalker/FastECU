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
            cancellation_.SetCancelled(true);
        if (write_error)
            return Fail(ErrorKind::kDisconnected, "write failed");
        return short_write ? request.size() - 1 : request.size();
    }
    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken&) override
    {
        trace.push_back("read");
        EXPECT_EQ(timeout, 1234ms);
        if (cancel_read)
            cancellation_.SetCancelled(true);
        if (read_error)
            return Fail(ErrorKind::kDisconnected, "read failed");
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
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_EQ(result.error().detail, details[checkpoint - 1]);
        EXPECT_EQ(cancellation.CheckCount(), checkpoint);
        std::vector<std::string> expected;
        if (checkpoint >= 2)
            expected.push_back("write");
        if (checkpoint >= 4)
            expected.push_back("read");
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
        ASSERT_FALSE(result.has_value());
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
                            return Fail(ErrorKind::kDisconnected, "sleep failed");
                        return {};
                    });
        }
        const auto result = DensoBeefKlineExchangeOptional(transport, scenario == 3 ? nullptr : &clock, cancellation,
                                                           bytes::Bytes{0xBE, 0xEF, 0, 1, 1, 0xAF},
                                                           scenario == 4 ? 0ms : 200ms, 1234ms);
        if (scenario == 2)
        {
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().detail, "sleep failed");
            EXPECT_EQ(result.error().kind, ErrorKind::kDisconnected);
            EXPECT_EQ(transport.trace, (std::vector<std::string>{"write", "sleep"}));
            EXPECT_EQ(cancellation.CheckCount(), 2U);
        }
        else
        {
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(result->has_value(), scenario == 1);
            if (scenario == 1)
                EXPECT_TRUE((**result).empty());
            EXPECT_EQ(transport.trace, (scenario < 3 ? std::vector<std::string>{"write", "sleep", "read"}
                                                     : std::vector<std::string>{"write", "read"}));
            EXPECT_EQ(cancellation.CheckCount(), 4U);
        }
    }
}
} // namespace
} // namespace fastecu::flash
