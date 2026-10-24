#include "src/backend/flash/guarded_can_flash_transport.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <tuple>

#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::ElementsAre;

class ProbeCanTransport : public ScriptedCanFlashTransport
{
  public:
    Status Write(bytes::ByteView data, const ICancellationToken& token) override
    {
        ++write_calls;
        last_write.assign(data.begin(), data.end());
        write_token = &token;
        if (cancel_inside != nullptr)
        {
            cancel_inside->Cancel();
        }
        return write_result;
    }

    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& token) override
    {
        ++read_calls;
        last_timeout = timeout;
        read_token = &token;
        if (cancel_inside != nullptr)
        {
            cancel_inside->Cancel();
        }
        return read_result;
    }

    Status RestartIso15765(const Iso15765Config& config, const ICancellationToken& token) override
    {
        ++restart_calls;
        restart_config = config;
        restart_token = &token;
        return Fail(ErrorKind::kUnsupported, "restart override");
    }

    void RequestUnblock() noexcept override
    {
        ++unblock_calls;
    }

    int write_calls = 0;
    int read_calls = 0;
    int restart_calls = 0;
    int unblock_calls = 0;
    bytes::Bytes last_write;
    std::chrono::milliseconds last_timeout{};
    const ICancellationToken *write_token = nullptr;
    const ICancellationToken *read_token = nullptr;
    const ICancellationToken *restart_token = nullptr;
    ManualCancellationToken *cancel_inside = nullptr;
    Iso15765Config restart_config{};
    Status write_result;
    Result<std::optional<bytes::Bytes>> read_result = std::optional<bytes::Bytes>{};
};

TEST(GuardedCanFlashTransport, PrecancelledWriteDoesNotDelegate)
{
    ProbeCanTransport inner;
    GuardedCanFlashTransport guard(inner);
    ManualCancellationToken token;
    token.Cancel();
    ASSERT_THAT(guard.Write(bytes::Bytes{0x12}, token),
                IsErrWith(ErrorKind::kCancelled, "CAN write cancelled before delegation"));
    EXPECT_EQ(inner.write_calls, 0);
}

TEST(GuardedCanFlashTransport, PrecancelledReadDoesNotDelegate)
{
    ProbeCanTransport inner;
    GuardedCanFlashTransport guard(inner);
    ManualCancellationToken token;
    token.Cancel();
    ASSERT_THAT(guard.Read(37ms, token), IsErrWith(ErrorKind::kCancelled, "CAN read cancelled before delegation"));
    EXPECT_EQ(inner.read_calls, 0);
}

TEST(GuardedCanFlashTransport, ForwardsBytesTimeoutAndOriginalToken)
{
    ProbeCanTransport inner;
    GuardedCanFlashTransport guard(inner);
    ManualCancellationToken token;
    ASSERT_THAT(guard.Write(bytes::Bytes{0x00, 0x80, 0xFF}, token), IsOk());
    ASSERT_THAT(guard.Read(37ms, token), IsOk());
    EXPECT_THAT(inner.last_write, ElementsAre(0x00, 0x80, 0xFF));
    EXPECT_EQ(inner.last_timeout, 37ms);
    EXPECT_EQ(inner.write_token, &token);
    EXPECT_EQ(inner.read_token, &token);
    EXPECT_EQ(inner.write_calls, 1);
    EXPECT_EQ(inner.read_calls, 1);
}

using ReadCase = std::tuple<int, bool>;
class GuardedCanFlashTransportReadOutcomes : public ::testing::TestWithParam<ReadCase>
{
};
TEST_P(GuardedCanFlashTransportReadOutcomes, ReadOutcomesPassThrough)
{
    const auto [outcome, cancel] = GetParam();
    const std::array<std::optional<bytes::Bytes>, 3> outcomes{std::nullopt, bytes::Bytes{}, bytes::Bytes{0x12, 0xA5}};
    ProbeCanTransport inner;
    GuardedCanFlashTransport guard(inner);
    ManualCancellationToken token;
    inner.read_result = outcomes.at(static_cast<std::size_t>(outcome));
    inner.cancel_inside = cancel ? &token : nullptr;
    const auto actual = guard.Read(37ms, token);
    ASSERT_THAT(actual, IsOk());
    EXPECT_EQ(*actual, outcomes.at(static_cast<std::size_t>(outcome)));
    EXPECT_EQ(token.Cancelled(), cancel);
    EXPECT_EQ(inner.read_calls, 1);
}
INSTANTIATE_TEST_SUITE_P(GuardedCanFlashTransport, GuardedCanFlashTransportReadOutcomes,
                         ::testing::Combine(::testing::Values(0, 1, 2), ::testing::Bool()),
                         (
                             [](const auto& param_info)
                             {
                                 const std::array<std::string, 3> names{"Absent", "Empty", "Data"};
                                 return names.at(static_cast<std::size_t>(std::get<0>(param_info.param))) +
                                        (std::get<1>(param_info.param) ? "CancelledInside" : "Live");
                             }));

using FailureCase = std::tuple<ErrorKind, bool, bool>;
class GuardedCanFlashTransportFailures : public ::testing::TestWithParam<FailureCase>
{
};
TEST_P(GuardedCanFlashTransportFailures, TypedFailuresPassThrough)
{
    const auto [kind, read, cancel] = GetParam();
    ProbeCanTransport inner;
    GuardedCanFlashTransport guard(inner);
    ManualCancellationToken token;
    inner.cancel_inside = cancel ? &token : nullptr;
    inner.write_result = Fail(kind, "delegate detail");
    inner.read_result = Fail(kind, "delegate detail");
    if (read)
    {
        ASSERT_THAT(guard.Read(37ms, token), IsErrWith(kind, "delegate detail"));
        EXPECT_EQ(inner.read_calls, 1);
        EXPECT_EQ(inner.write_calls, 0);
    }
    else
    {
        ASSERT_THAT(guard.Write(bytes::Bytes{0x12}, token), IsErrWith(kind, "delegate detail"));
        EXPECT_EQ(inner.write_calls, 1);
        EXPECT_EQ(inner.read_calls, 0);
    }
    EXPECT_EQ(token.Cancelled(), cancel);
}
INSTANTIATE_TEST_SUITE_P(GuardedCanFlashTransport, GuardedCanFlashTransportFailures,
                         ::testing::Combine(::testing::Values(ErrorKind::kInvalidConfig, ErrorKind::kTimeout,
                                                              ErrorKind::kDisconnected, ErrorKind::kBadResponse,
                                                              ErrorKind::kCancelled, ErrorKind::kUnsupported,
                                                              ErrorKind::kInternal),
                                            ::testing::Bool(), ::testing::Bool()),
                         [](const auto& param_info)
                         {
                             return std::string(ToString(std::get<0>(param_info.param))) +
                                    (std::get<1>(param_info.param) ? "Read" : "Write") +
                                    (std::get<2>(param_info.param) ? "CancelledInside" : "Live");
                         });

TEST(GuardedCanFlashTransport, AcceptedWriteSuccessSurvivesInsideCallCancellation)
{
    ProbeCanTransport inner;
    GuardedCanFlashTransport guard(inner);
    ManualCancellationToken token;
    inner.cancel_inside = &token;
    ASSERT_THAT(guard.Write(bytes::Bytes{0x12}, token), IsOk());
    EXPECT_TRUE(token.Cancelled());
    EXPECT_EQ(inner.write_calls, 1);
}

TEST(GuardedCanFlashTransport, FailureDoesNotPoisonTheNextCall)
{
    ProbeCanTransport inner;
    GuardedCanFlashTransport guard(inner);
    ManualCancellationToken token;
    inner.write_result = Fail(ErrorKind::kDisconnected);
    ASSERT_THAT(guard.Write(bytes::Bytes{0x12}, token), IsErr(ErrorKind::kDisconnected));
    inner.write_result = {};
    ASSERT_THAT(guard.Write(bytes::Bytes{0x12}, token), IsOk());
    EXPECT_EQ(inner.write_calls, 2);
}

void ExpectConfig(const Iso15765Config& actual)
{
    EXPECT_EQ(actual.bitrate, 500000);
    EXPECT_EQ(actual.request_id, 0x7E0U);
    EXPECT_EQ(actual.response_id, 0x7E8U);
    EXPECT_FALSE(actual.extended_id);
}

class GuardedCanFlashTransportLifecycle : public ::testing::TestWithParam<bool>
{
};
TEST_P(GuardedCanFlashTransportLifecycle, LifecycleRestartAndUnblockForwardAfterCancellation)
{
    ProbeCanTransport inner;
    GuardedCanFlashTransport guard(inner);
    ManualCancellationToken token;
    token.Cancel();
    if (GetParam())
    {
        inner.reset_result = Fail(ErrorKind::kDisconnected, "reset detail");
        inner.configure_result = Fail(ErrorKind::kInvalidConfig, "configure detail");
        inner.open_result = Fail(ErrorKind::kTimeout, "open detail");
        inner.close_result = Fail(ErrorKind::kInternal, "close detail");
    }
    const Iso15765Config config{500000, 0x7E0, 0x7E8, false};
    if (GetParam())
    {
        EXPECT_THAT(guard.ResetConnection(), IsErrWith(ErrorKind::kDisconnected, "reset detail"));
        EXPECT_THAT(guard.Configure(config), IsErrWith(ErrorKind::kInvalidConfig, "configure detail"));
        EXPECT_THAT(guard.Open(), IsErrWith(ErrorKind::kTimeout, "open detail"));
        EXPECT_THAT(guard.Close(), IsErrWith(ErrorKind::kInternal, "close detail"));
    }
    else
    {
        EXPECT_THAT(guard.ResetConnection(), IsOk());
        EXPECT_THAT(guard.Configure(config), IsOk());
        EXPECT_THAT(guard.Open(), IsOk());
        EXPECT_THAT(guard.Close(), IsOk());
    }
    ASSERT_THAT(guard.RestartIso15765(config, token), IsErrWith(ErrorKind::kUnsupported, "restart override"));
    guard.RequestUnblock();
    EXPECT_THAT(inner.lifecycle_calls, ElementsAre("reset_connection", "configure", "open", "close"));
    EXPECT_EQ(inner.reset_call_count, 1);
    EXPECT_EQ(inner.configure_call_count, 1);
    EXPECT_EQ(inner.open_call_count, 1);
    EXPECT_EQ(inner.close_call_count, 1);
    ASSERT_TRUE(inner.last_config.has_value());
    ExpectConfig(*inner.last_config);
    ExpectConfig(inner.restart_config);
    EXPECT_EQ(inner.restart_token, &token);
    EXPECT_EQ(inner.restart_calls, 1);
    EXPECT_EQ(inner.unblock_calls, 1);
}
INSTANTIATE_TEST_SUITE_P(GuardedCanFlashTransport, GuardedCanFlashTransportLifecycle, ::testing::Bool(),
                         [](const auto& param_info) { return param_info.param ? "Failures" : "Success"; });

} // namespace
} // namespace fastecu::flash
