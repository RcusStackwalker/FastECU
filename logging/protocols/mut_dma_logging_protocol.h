#pragma once
#include <memory>
#include "logging/logging_protocol.h"
#include "protocol/ikline_transport.h"
#include "protocol/imut_dma_init.h"
#include "protocol/mut_dma_driver.h"
#include <file_actions.h>

class MutDmaLoggingProtocol : public LoggingProtocol {
public:
    MutDmaLoggingProtocol(std::unique_ptr<mutdma::IKlineTransport> transport,
                           std::unique_ptr<mutdma::IMutDmaInit> init,
                           FileActions::LogValuesStructure *logValues, FileActions *fileActions);

    bool start(QString *errorOut) override;
    PollResult poll(int timeoutMs) override;
    void stop() override;

private:
    std::unique_ptr<mutdma::IKlineTransport> transport_;
    std::unique_ptr<mutdma::IMutDmaInit> init_;
    FileActions::LogValuesStructure *logValues_;
    FileActions *fileActions_;
    mutdma::MutDmaDriver driver_;
    QVector<mutdma::Channel> channels_;
    QVector<int> channelLogValueIndex_;
};
