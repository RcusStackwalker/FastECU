#pragma once
#include <memory>
#include "src/backend/logging/logging_protocol.h"
#include "src/backend/protocol/ikline_transport.h"
#include "src/backend/protocol/imut_dma_init.h"
#include "src/backend/protocol/mut_dma_driver.h"
#include "src/backend/definitions/file_actions.h"

class MutDmaLoggingProtocol : public LoggingProtocol
{
  public:
    MutDmaLoggingProtocol(std::unique_ptr<mutdma::IKlineTransport> transport,
                          std::unique_ptr<mutdma::IMutDmaInit> init,
                          FileActions::LogValuesStructure *logValues, FileActions *fileActions);

    fastecu::Status start() override;
    fastecu::Result<PollData> poll(int timeoutMs) override;
    void stop() override;

  private:
    std::unique_ptr<mutdma::IKlineTransport> transport_;
    std::unique_ptr<mutdma::IMutDmaInit> init_;
    FileActions::LogValuesStructure *logValues_;
    FileActions *fileActions_;
    mutdma::MutDmaDriver driver_;
    QVector<int> channelLogValueIndex_;
};
