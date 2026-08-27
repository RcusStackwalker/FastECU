#include "src/platform/desktop/common/connection/socketcan_transport.h"

#include "src/backend/ports/testing/fake_cancellation_token.h"

#include <gtest/gtest.h>

#include <linux/can.h>
#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>

namespace fastecu::desktop::connection
{
namespace
{

class SocketCanHarness
{
  public:
    detail::SocketCanActions actions()
    {
        return {
            .send =
                [this](int fd, const void *buffer, std::size_t size, int flags)
            {
                send_fd = fd;
                send_flags = flags;
                std::memcpy(&sent_frame, buffer, std::min(size, sizeof(sent_frame)));
                return send_result.value_or(static_cast<std::ptrdiff_t>(size));
            },
            .poll =
                [this](int fd, short events, int timeout_ms, short& revents)
            {
                ++poll_calls;
                poll_fd = fd;
                poll_events = events;
                poll_timeout_ms = timeout_ms;
                revents = poll_revents;
                if (poll_result < 0)
                {
                    errno = poll_errno;
                }
                return poll_result;
            },
            .recv =
                [this](int fd, void *buffer, std::size_t size, int flags)
            {
                ++recv_calls;
                recv_fd = fd;
                recv_flags = flags;
                if (recv_result < 0)
                {
                    errno = recv_errno;
                    return recv_result;
                }
                if (recv_result > 0)
                {
                    std::memcpy(buffer, &received_frame, std::min(size, sizeof(received_frame)));
                }
                return recv_result;
            },
            .close =
                [this](int fd)
            {
                ++close_calls;
                closed_fd = fd;
                return 0;
            },
        };
    }

    can_frame sent_frame{};
    can_frame received_frame{};
    std::optional<std::ptrdiff_t> send_result;
    std::ptrdiff_t recv_result{static_cast<std::ptrdiff_t>(sizeof(can_frame))};
    int recv_errno{0};
    int poll_result{1};
    int poll_errno{0};
    short poll_revents{POLLIN};
    int send_fd{-1};
    int send_flags{-1};
    int poll_fd{-1};
    short poll_events{0};
    int poll_timeout_ms{-1};
    int recv_fd{-1};
    int recv_flags{-1};
    int closed_fd{-1};
    int poll_calls{0};
    int recv_calls{0};
    int close_calls{0};
};

TEST(SocketCanTransport, StandardIdentifiersAreSentWithoutTheExtendedFlag)
{
    SocketCanHarness harness;
    SocketCanTransport transport(17, dashboard::CanIdentifierWidth::Standard, harness.actions());

    const bytes::Bytes payload{0x10, 0x20, 0x30};
    const auto result = transport.write(0x5A3, payload);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, payload.size());
    EXPECT_EQ(harness.sent_frame.can_id, 0x5A3U);
    EXPECT_EQ(harness.sent_frame.can_id & CAN_EFF_FLAG, 0U);
    EXPECT_EQ(harness.sent_frame.can_dlc, payload.size());
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), harness.sent_frame.data));
}

TEST(SocketCanTransport, ExtendedIdentifiersAreSentWithTheExtendedFlag)
{
    SocketCanHarness harness;
    SocketCanTransport transport(18, dashboard::CanIdentifierWidth::Extended, harness.actions());

    const auto result = transport.write(0x1ABCDE3, bytes::Bytes{0x7E});

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(harness.sent_frame.can_id, (0x1ABCDE3U | CAN_EFF_FLAG));
}

TEST(SocketCanTransport, PayloadsLargerThanAClassicCanFrameAreRejected)
{
    SocketCanHarness harness;
    SocketCanTransport transport(19, dashboard::CanIdentifierWidth::Standard, harness.actions());

    const auto result = transport.write(0x123, bytes::Bytes(9, 0xAA));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(harness.sent_frame.can_dlc, 0);
}

TEST(SocketCanTransport, ReadStripsSocketCanFlagsAndPreservesPayload)
{
    SocketCanHarness harness;
    harness.received_frame.can_id = 0x1ABCDE3U | CAN_EFF_FLAG | CAN_RTR_FLAG;
    harness.received_frame.can_dlc = 3;
    harness.received_frame.data[0] = 0xDE;
    harness.received_frame.data[1] = 0xAD;
    harness.received_frame.data[2] = 0xBE;
    SocketCanTransport transport(20, dashboard::CanIdentifierWidth::Extended, harness.actions());
    FakeCancellationToken active;

    const auto result = transport.read(25, active);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->id, 0x1ABCDE3U);
    EXPECT_EQ((*result)->payload, (bytes::Bytes{0xDE, 0xAD, 0xBE}));
}

TEST(SocketCanTransport, ReadLeavesAnUnrelatedIdentifierForProtocolLevelFiltering)
{
    SocketCanHarness harness;
    harness.received_frame.can_id = 0x456;
    harness.received_frame.can_dlc = 1;
    harness.received_frame.data[0] = 0x91;
    SocketCanTransport transport(21, dashboard::CanIdentifierWidth::Standard, harness.actions());
    FakeCancellationToken active;

    const auto result = transport.read(25, active);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->id, 0x456U);
    EXPECT_EQ((*result)->payload, (bytes::Bytes{0x91}));
}

TEST(SocketCanTransport, PollTimeoutReturnsAnEmptyOptional)
{
    SocketCanHarness harness;
    harness.poll_result = 0;
    harness.poll_revents = 0;
    SocketCanTransport transport(22, dashboard::CanIdentifierWidth::Standard, harness.actions());
    FakeCancellationToken active;

    const auto result = transport.read(25, active);

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
    EXPECT_EQ(harness.poll_calls, 1);
    EXPECT_EQ(harness.poll_timeout_ms, 25);
}

TEST(SocketCanTransport, CancellationIsReturnedBeforePolling)
{
    SocketCanHarness harness;
    SocketCanTransport transport(23, dashboard::CanIdentifierWidth::Standard, harness.actions());
    FakeCancellationToken cancelled(true);

    const auto result = transport.read(100, cancelled);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(harness.poll_calls, 0);
}

TEST(SocketCanTransport, PollHangupIsDisconnectedWithoutReceiving)
{
    SocketCanHarness harness;
    harness.poll_revents = POLLHUP;
    SocketCanTransport transport(24, dashboard::CanIdentifierWidth::Standard, harness.actions());
    FakeCancellationToken active;

    const auto result = transport.read(100, active);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(harness.recv_calls, 0);
}

TEST(SocketCanTransport, DeviceRemovalDuringReceiveIsDisconnected)
{
    SocketCanHarness harness;
    harness.recv_result = -1;
    harness.recv_errno = ENODEV;
    SocketCanTransport transport(25, dashboard::CanIdentifierWidth::Standard, harness.actions());
    FakeCancellationToken active;

    const auto result = transport.read(100, active);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST(SocketCanTransport, ZeroByteReceiveIsDisconnected)
{
    SocketCanHarness harness;
    harness.recv_result = 0;
    SocketCanTransport transport(26, dashboard::CanIdentifierWidth::Standard, harness.actions());
    FakeCancellationToken active;

    const auto result = transport.read(100, active);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST(SocketCanTransport, DestructionClosesTheOwnedDescriptorExactlyOnce)
{
    SocketCanHarness harness;
    {
        SocketCanTransport transport(27, dashboard::CanIdentifierWidth::Standard, harness.actions());
        EXPECT_TRUE(transport.isOpen());
        EXPECT_EQ(harness.close_calls, 0);
    }

    EXPECT_EQ(harness.close_calls, 1);
    EXPECT_EQ(harness.closed_fd, 27);
}

} // namespace
} // namespace fastecu::desktop::connection
