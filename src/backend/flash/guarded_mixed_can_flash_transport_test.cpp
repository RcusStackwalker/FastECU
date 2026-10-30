#include "src/backend/flash/guarded_mixed_can_flash_transport.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::ElementsAre;

// Intentionally blind to tokens: records the actual entry and delegated outcome.
class BlindMixedTransport final : public IMixedCanFlashTransport
{
  public:
    Status ResetConnection() override
    {
        return Control("reset");
    }
    Status Configure(const MixedCanConfig& config) override
    {
        last_config = config;
        return Control("configure");
    }
    Status Open() override
    {
        return Control("open");
    }
    Status Close() override
    {
        return Control("close");
    }
    Status EnterRawBootloaderMode() override
    {
        return Control("raw");
    }
    Status ClearReceiveBuffer() override
    {
        return Control("clear");
    }
    Status EnterIso15765KernelMode() override
    {
        return Control("iso");
    }
    void RequestUnblock() noexcept override
    {
        ++unblocks;
    }
    Status WriteIso15765(bytes::ByteView data, const ICancellationToken& token) override
    {
        last_bytes.assign(data.begin(), data.end());
        Enter(token);
        return write_result;
    }
    Result<std::optional<bytes::Bytes>> ReadIso15765(std::chrono::milliseconds timeout,
                                                     const ICancellationToken& token) override
    {
        last_timeout = timeout;
        Enter(token);
        return iso_result;
    }
    Status WriteRaw(const cdbg::CanFrame& frame, const ICancellationToken& token) override
    {
        last_frame = frame;
        Enter(token);
        return write_result;
    }
    Result<std::optional<cdbg::CanFrame>> ReadRaw(std::chrono::milliseconds timeout,
                                                  const ICancellationToken& token) override
    {
        last_timeout = timeout;
        Enter(token);
        return raw_result;
    }
    Status Control(std::string name)
    {
        controls.push_back(std::move(name));
        return control_result;
    }
    void Enter(const ICancellationToken& token)
    {
        ++attempts;
        last_token = &token;
        if (cancel_inside != nullptr)
        {
            cancel_inside->Cancel();
        }
    }
    int attempts = 0;
    int unblocks = 0;
    const ICancellationToken *last_token = nullptr;
    ManualCancellationToken *cancel_inside = nullptr;
    bytes::Bytes last_bytes;
    cdbg::CanFrame last_frame{};
    std::chrono::milliseconds last_timeout{};
    MixedCanConfig last_config{};
    std::vector<std::string> controls;
    Status control_result;
    Status write_result;
    Result<std::optional<bytes::Bytes>> iso_result = std::optional<bytes::Bytes>{};
    Result<std::optional<cdbg::CanFrame>> raw_result = std::optional<cdbg::CanFrame>{};
};

constexpr std::array<ErrorKind, 7> kErrorKinds{
    ErrorKind::kInvalidConfig, ErrorKind::kTimeout,     ErrorKind::kDisconnected, ErrorKind::kBadResponse,
    ErrorKind::kCancelled,     ErrorKind::kUnsupported, ErrorKind::kInternal};
const bytes::Bytes kBytes{0x00, 0x80, 0xFF};
const cdbg::CanFrame kFrame{.id = 0x1ABCDE, .payload = kBytes};

// Normalize only the test's result shape; delegated values are checked below.
Status Invoke(GuardedMixedCanFlashTransport& guard, int operation, const ICancellationToken& token)
{
    if (operation == 0)
    {
        return guard.WriteIso15765(kBytes, token);
    }
    if (operation == 1)
    {
        const auto result = guard.ReadIso15765(37ms, token);
        if (result.has_value())
        {
            return {};
        }
        return std::unexpected(result.error());
    }
    if (operation == 2)
    {
        return guard.WriteRaw(kFrame, token);
    }
    const auto result = guard.ReadRaw(37ms, token);
    if (result.has_value())
    {
        return {};
    }
    return std::unexpected(result.error());
}

class GuardedMixedCanIo : public ::testing::TestWithParam<int>
{
};
TEST_P(GuardedMixedCanIo, PrecancelledIoDoesNotDelegate)
{
    BlindMixedTransport inner;
    GuardedMixedCanFlashTransport guard(inner);
    ManualCancellationToken token;
    token.Cancel();
    const std::array<std::string, 4> details{
        "mixed CAN ISO-15765 write cancelled before delegation", "mixed CAN ISO-15765 read cancelled before delegation",
        "mixed CAN raw write cancelled before delegation", "mixed CAN raw read cancelled before delegation"};
    EXPECT_THAT(Invoke(guard, GetParam(), token),
                IsErrWith(ErrorKind::kCancelled, details.at(static_cast<std::size_t>(GetParam()))));
    EXPECT_EQ(inner.attempts, 0);
}
TEST_P(GuardedMixedCanIo, DelegatedOutcomeAndArgumentsAreUnchanged)
{
    for (const bool cancel : {false, true})
    {
        SCOPED_TRACE(::testing::Message() << "cancel=" << cancel);
        for (int outcome = 0; outcome < 10; ++outcome)
        {
            SCOPED_TRACE(::testing::Message() << "outcome=" << outcome << " cancel=" << cancel);
            BlindMixedTransport inner;
            GuardedMixedCanFlashTransport guard(inner);
            ManualCancellationToken token;
            inner.cancel_inside = cancel ? &token : nullptr;
            if (outcome >= 3)
            {
                const auto kind = kErrorKinds.at(static_cast<std::size_t>(outcome - 3));
                inner.write_result = Fail(kind, "original detail");
                inner.iso_result = Fail(kind, "original detail");
                inner.raw_result = Fail(kind, "original detail");
                EXPECT_THAT(Invoke(guard, GetParam(), token), IsErrWith(kind, "original detail"));
            }
            else if (GetParam() == 1)
            {
                const std::optional<bytes::Bytes> expected =
                    outcome == 0 ? std::nullopt : std::optional<bytes::Bytes>(outcome == 1 ? bytes::Bytes{} : kBytes);
                inner.iso_result = expected;
                const auto result = guard.ReadIso15765(37ms, token);
                ASSERT_THAT(result, IsOk());
                EXPECT_EQ(*result, expected);
            }
            else if (GetParam() == 3)
            {
                const std::optional<cdbg::CanFrame> expected =
                    outcome == 0 ? std::nullopt
                                 : std::optional<cdbg::CanFrame>(cdbg::CanFrame{
                                       .id = kFrame.id, .payload = outcome == 1 ? bytes::Bytes{} : kBytes});
                inner.raw_result = expected;
                const auto result = guard.ReadRaw(37ms, token);
                ASSERT_THAT(result, IsOk());
                ASSERT_EQ(result->has_value(), expected.has_value());
                if (expected.has_value())
                {
                    EXPECT_EQ(result->value().id, expected->id);
                    EXPECT_EQ(result->value().payload, expected->payload);
                }
            }
            else
            {
                EXPECT_THAT(Invoke(guard, GetParam(), token), IsOk());
            }
            EXPECT_EQ(inner.attempts, 1);
            EXPECT_EQ(inner.last_token, &token);
            EXPECT_EQ(token.Cancelled(), cancel);
            if (GetParam() == 0)
            {
                EXPECT_EQ(inner.last_bytes, kBytes);
            }
            else if (GetParam() == 2)
            {
                EXPECT_EQ(inner.last_frame.id, kFrame.id);
                EXPECT_EQ(inner.last_frame.payload, kBytes);
            }
            else
            {
                EXPECT_EQ(inner.last_timeout, 37ms);
            }
        }
    }
}
TEST_P(GuardedMixedCanIo, FailureDoesNotPoisonNextCall)
{
    BlindMixedTransport inner;
    GuardedMixedCanFlashTransport guard(inner);
    ManualCancellationToken token;
    inner.write_result = Fail(ErrorKind::kDisconnected, "first failure");
    inner.iso_result = Fail(ErrorKind::kDisconnected, "first failure");
    inner.raw_result = Fail(ErrorKind::kDisconnected, "first failure");
    EXPECT_THAT(Invoke(guard, GetParam(), token), IsErrWith(ErrorKind::kDisconnected, "first failure"));
    inner.write_result = {};
    inner.iso_result = std::optional<bytes::Bytes>{};
    inner.raw_result = std::optional<cdbg::CanFrame>{};
    EXPECT_THAT(Invoke(guard, GetParam(), token), IsOk());
    EXPECT_EQ(inner.attempts, 2);
}
INSTANTIATE_TEST_SUITE_P(GuardedMixedCanFlashTransport, GuardedMixedCanIo, ::testing::Values(0, 1, 2, 3),
                         (
                             [](const auto& param_info)
                             {
                                 const std::array<std::string, 4> names{"IsoWrite", "IsoRead", "RawWrite", "RawRead"};
                                 return names.at(static_cast<std::size_t>(param_info.param));
                             }));

TEST(GuardedMixedCanFlashTransport, ControlOperationsStillDelegateAfterCancellation)
{
    for (int outcome = -1; outcome < 7; ++outcome)
    {
        SCOPED_TRACE(::testing::Message() << "outcome=" << outcome);
        BlindMixedTransport inner;
        GuardedMixedCanFlashTransport guard(inner);
        ManualCancellationToken token;
        token.Cancel();
        if (outcome >= 0)
        {
            inner.control_result = Fail(kErrorKinds.at(static_cast<std::size_t>(outcome)), "control detail");
        }
        const MixedCanConfig config{{500000, 0x7E0, 0x7E8, false}, {250000, 0xFFFFE, 0x21, true}};
        const std::array<Status, 7> results{
            guard.ResetConnection(),    guard.Configure(config),         guard.Open(), guard.EnterRawBootloaderMode(),
            guard.ClearReceiveBuffer(), guard.EnterIso15765KernelMode(), guard.Close()};
        for (std::size_t index = 0; index < results.size(); ++index)
        {
            SCOPED_TRACE(inner.controls.at(index));
            if (outcome < 0)
            {
                EXPECT_THAT(results.at(index), IsOk());
            }
            else
            {
                EXPECT_THAT(results.at(index),
                            IsErrWith(kErrorKinds.at(static_cast<std::size_t>(outcome)), "control detail"));
            }
        }
        guard.RequestUnblock();
        EXPECT_THAT(inner.controls, ElementsAre("reset", "configure", "open", "raw", "clear", "iso", "close"));
        EXPECT_EQ(inner.unblocks, 1);
        EXPECT_EQ(inner.last_config.kernel.bitrate, 500000);
        EXPECT_EQ(inner.last_config.kernel.request_id, 0x7E0U);
        EXPECT_EQ(inner.last_config.kernel.response_id, 0x7E8U);
        EXPECT_FALSE(inner.last_config.kernel.extended_id);
        EXPECT_EQ(inner.last_config.bootloader.bitrate, 250000);
        EXPECT_EQ(inner.last_config.bootloader.transmit_id, 0xFFFFEU);
        EXPECT_EQ(inner.last_config.bootloader.receive_id, 0x21U);
        EXPECT_TRUE(inner.last_config.bootloader.extended_id);
        EXPECT_EQ(inner.attempts, 0);
    }
}

} // namespace
} // namespace fastecu::flash
