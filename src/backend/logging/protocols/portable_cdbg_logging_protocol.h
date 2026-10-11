#pragma once

#include <chrono>
#include <memory>
#include <vector>

#include "src/backend/logging/logging_protocol.h"
#include "src/backend/protocol/ican_transport.h"
#include "src/backend/protocol/mitsu_colt_can_cdbg_driver.h"

namespace fastecu::logging
{

class CdbgLoggingProtocol final : public LoggingProtocol
{
  public:
    CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport, std::vector<LoggingChannel> channels);

    fastecu::Status Start(const fastecu::ICancellationToken& cancellation) override;
    fastecu::Result<PollData> Poll(std::chrono::milliseconds timeout,
                                   const fastecu::ICancellationToken& cancellation) override;
    fastecu::Status Stop() override;

  private:
    std::unique_ptr<cdbg::ICanTransport> transport_;
    const std::vector<LoggingChannel> channels_;
    const std::vector<mitsu_colt_can_cdbg::CdbgChannel> wire_channels_;
    mitsu_colt_can_cdbg::CdbgLogDriver driver_;
};

} // namespace fastecu::logging
