#pragma once

#include <memory>
#include <vector>

#include "src/backend/logging/portable_logging_protocol.h"
#include "src/backend/protocol/ican_transport.h"
#include "src/backend/protocol/mitsu_colt_can_cdbg_driver.h"

namespace fastecu::logging
{

class CdbgLoggingProtocol final : public LoggingProtocol
{
  public:
    CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport,
                        std::vector<LoggingChannel> channels);

    fastecu::Status start(const fastecu::ICancellationToken& cancellation) override;
    fastecu::Result<PollData> poll(
        int timeout_ms, const fastecu::ICancellationToken& cancellation) override;
    fastecu::Status stop() override;

  private:
    std::unique_ptr<cdbg::ICanTransport> transport_;
    const std::vector<LoggingChannel> channels_;
    const std::vector<MitsuColtCanCdbg::CdbgChannel> wire_channels_;
    MitsuColtCanCdbg::CdbgLogDriver driver_;
};

} // namespace fastecu::logging
