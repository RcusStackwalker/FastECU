#pragma once

#include <memory>
#include <vector>

#include "src/backend/logging/portable_logging_protocol.h"
#include "src/backend/protocol/ikline_transport.h"
#include "src/backend/protocol/imut_dma_init.h"
#include "src/backend/protocol/mut_dma_driver.h"

namespace fastecu::logging
{

class MutDmaLoggingProtocol final : public LoggingProtocol
{
  public:
    MutDmaLoggingProtocol(std::unique_ptr<mutdma::IKlineTransport> transport,
                          std::unique_ptr<mutdma::IMutDmaInit> init,
                          std::vector<LoggingChannel> channels);

    fastecu::Status start(const fastecu::ICancellationToken& cancellation) override;
    fastecu::Result<PollData> poll(
        int timeout_ms, const fastecu::ICancellationToken& cancellation) override;
    fastecu::Status stop() override;

  private:
    std::unique_ptr<mutdma::IKlineTransport> transport_;
    std::unique_ptr<mutdma::IMutDmaInit> init_;
    const std::vector<LoggingChannel> channels_;
    const std::vector<mutdma::Channel> wire_channels_;
    mutdma::MutDmaDriver driver_;
};

} // namespace fastecu::logging
