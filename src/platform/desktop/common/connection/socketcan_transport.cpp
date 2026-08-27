#include "src/platform/desktop/common/connection/socketcan_transport.h"

#include <linux/can.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace fastecu::desktop::connection
{
namespace
{

constexpr int kCancellationPollSliceMs = 50;

Error syscall_error(std::string operation, int error_number)
{
    switch (error_number)
    {
    case EBADF:
    case ENETDOWN:
    case ENETRESET:
    case ENETUNREACH:
    case ENODEV:
    case ENXIO:
    case EPIPE:
        return {ErrorKind::Disconnected, std::move(operation) + " failed: adapter disconnected"};
    default:
        return {ErrorKind::Internal, std::move(operation) + " failed"};
    }
}

Result<std::uint32_t> encoded_identifier(std::uint32_t can_id, dashboard::CanIdentifierWidth width)
{
    switch (width)
    {
    case dashboard::CanIdentifierWidth::Standard:
        if (can_id > CAN_SFF_MASK)
        {
            return fail(ErrorKind::InvalidConfig, "standard CAN identifier exceeds 11 bits");
        }
        return can_id;
    case dashboard::CanIdentifierWidth::Extended:
        if (can_id > CAN_EFF_MASK)
        {
            return fail(ErrorKind::InvalidConfig, "extended CAN identifier exceeds 29 bits");
        }
        return can_id | CAN_EFF_FLAG;
    }
    return fail(ErrorKind::InvalidConfig, "unknown CAN identifier width");
}

} // namespace

namespace detail
{

SocketCanActions production_socketcan_actions()
{
    return {
        .send = [](int fd, const void *buffer, std::size_t size, int flags)
        { return static_cast<std::ptrdiff_t>(::send(fd, buffer, size, flags)); },
        .poll =
            [](int fd, short events, int timeout_ms, short& revents)
        {
            pollfd descriptor{.fd = fd, .events = events, .revents = 0};
            const int result = ::poll(&descriptor, 1, timeout_ms);
            revents = descriptor.revents;
            return result;
        },
        .recv = [](int fd, void *buffer, std::size_t size, int flags)
        { return static_cast<std::ptrdiff_t>(::recv(fd, buffer, size, flags)); },
        .close = [](int fd) { return ::close(fd); },
    };
}

} // namespace detail

SocketCanTransport::SocketCanTransport(int fd, dashboard::CanIdentifierWidth width, detail::SocketCanActions actions)
    : fd_(fd), width_(width), actions_(std::move(actions))
{
}

SocketCanTransport::~SocketCanTransport()
{
    if (fd_ >= 0 && actions_.close)
    {
        actions_.close(fd_);
        fd_ = -1;
    }
}

Result<std::size_t> SocketCanTransport::write(std::uint32_t can_id, bytes::ByteView payload)
{
    if (!isOpen())
    {
        return fail(ErrorKind::Disconnected, "SocketCAN adapter is closed");
    }
    if (!actions_.send)
    {
        return fail(ErrorKind::Internal, "SocketCAN send action is unavailable");
    }
    if (payload.size() > CAN_MAX_DLEN)
    {
        return fail(ErrorKind::InvalidConfig, "classic CAN payload exceeds eight bytes");
    }

    const auto encoded = encoded_identifier(can_id, width_);
    if (!encoded.has_value())
    {
        return std::unexpected(encoded.error());
    }

    can_frame frame{};
    frame.can_id = *encoded;
    frame.can_dlc = static_cast<decltype(frame.can_dlc)>(payload.size());
    std::copy(payload.begin(), payload.end(), frame.data);

    const std::ptrdiff_t sent = actions_.send(fd_, &frame, sizeof(frame), 0);
    if (sent < 0)
    {
        return std::unexpected(syscall_error("SocketCAN send", errno));
    }
    if (sent == 0)
    {
        return fail(ErrorKind::Disconnected, "SocketCAN send returned zero bytes");
    }
    if (sent != static_cast<std::ptrdiff_t>(sizeof(frame)))
    {
        return fail(ErrorKind::Internal, "SocketCAN sent a partial CAN frame");
    }
    return payload.size();
}

Result<std::optional<cdbg::CanFrame>> SocketCanTransport::read(int timeout_ms, const ICancellationToken& cancellation)
{
    if (!isOpen())
    {
        return fail(ErrorKind::Disconnected, "SocketCAN adapter is closed");
    }
    if (!actions_.poll || !actions_.recv)
    {
        return fail(ErrorKind::Internal, "SocketCAN read actions are unavailable");
    }

    int remaining_ms = std::max(timeout_ms, 0);
    while (true)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "SocketCAN read cancelled");
        }

        const int slice_ms = std::min(remaining_ms, kCancellationPollSliceMs);
        short revents = 0;
        const int polled = actions_.poll(fd_, POLLIN, slice_ms, revents);
        if (polled < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return std::unexpected(syscall_error("SocketCAN poll", errno));
        }
        if (polled == 0)
        {
            if (remaining_ms <= slice_ms)
            {
                return std::optional<cdbg::CanFrame>{};
            }
            remaining_ms -= slice_ms;
            continue;
        }
        if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return fail(ErrorKind::Disconnected, "SocketCAN poll reported a disconnected adapter");
        }
        if ((revents & POLLIN) == 0)
        {
            return fail(ErrorKind::Internal, "SocketCAN poll returned without readable data");
        }

        can_frame frame{};
        const std::ptrdiff_t received = actions_.recv(fd_, &frame, sizeof(frame), 0);
        if (received < 0)
        {
            return std::unexpected(syscall_error("SocketCAN receive", errno));
        }
        if (received == 0)
        {
            return fail(ErrorKind::Disconnected, "SocketCAN receive returned zero bytes");
        }
        if (received != static_cast<std::ptrdiff_t>(sizeof(frame)) || frame.can_dlc > CAN_MAX_DLEN)
        {
            return fail(ErrorKind::Internal, "SocketCAN received a malformed CAN frame");
        }

        const bool extended = (frame.can_id & CAN_EFF_FLAG) != 0;
        cdbg::CanFrame decoded{
            .id = frame.can_id & (extended ? CAN_EFF_MASK : CAN_SFF_MASK),
            .payload = bytes::Bytes(frame.data, frame.data + frame.can_dlc),
        };
        return std::optional<cdbg::CanFrame>{std::move(decoded)};
    }
}

bool SocketCanTransport::isOpen() const
{
    return fd_ >= 0;
}

} // namespace fastecu::desktop::connection
