#pragma once

#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/protocol/ican_transport.h"
#include "src/platform/desktop/common/connection/socketcan_actions.h"

namespace fastecu::desktop::connection
{
class SocketCanTransport final : public cdbg::ICanTransport
{
  public:
    SocketCanTransport(int fd, dashboard::CanIdentifierWidth width, detail::SocketCanActions actions);
    ~SocketCanTransport() override;

    SocketCanTransport(const SocketCanTransport&) = delete;
    SocketCanTransport& operator=(const SocketCanTransport&) = delete;
    SocketCanTransport(SocketCanTransport&&) = delete;
    SocketCanTransport& operator=(SocketCanTransport&&) = delete;

    Result<std::size_t> write(std::uint32_t can_id, bytes::ByteView payload) override;
    Result<std::optional<cdbg::CanFrame>> read(int timeout_ms, const ICancellationToken& cancellation) override;
    bool isOpen() const override;

  private:
    int fd_;
    dashboard::CanIdentifierWidth width_;
    detail::SocketCanActions actions_;
};

} // namespace fastecu::desktop::connection
