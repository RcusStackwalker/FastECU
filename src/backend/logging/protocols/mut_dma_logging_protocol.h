#pragma once
#include <memory>
#include "src/backend/logging/logging_protocol.h"
#include "src/backend/logging/protocols/portable_mut_dma_logging_protocol.h"
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
    FileActions::LogValuesStructure *logValues_;
    FileActions *fileActions_;
    QVector<int> channelLogValueIndex_;
    std::unique_ptr<fastecu::logging::MutDmaLoggingProtocol> core_;
};
