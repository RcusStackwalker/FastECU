#pragma once
#include <memory>
#include <QString>
#include "src/backend/logging/logging_protocol.h"
#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"
#include "src/backend/definitions/file_actions.h"

class SsmLoggingProtocol : public LoggingProtocol
{
  public:
    SsmLoggingProtocol(fastecu::IClock& clock, std::unique_ptr<ISsmTransport> transport,
                       FileActions::LogValuesStructure *logValues, FileActions *fileActions,
                       QString logValueProtocolFilter, bool targetIsEcu, bool useOpenport2Adapter);

    fastecu::Status start() override;
    fastecu::Result<PollData> poll(int timeoutMs) override;
    void stop() override;

  private:
    FileActions::LogValuesStructure *logValues_;
    FileActions *fileActions_;
    QVector<int> channelLogValueIndex_;
    QVector<bool> channelEnabled_;
    std::unique_ptr<fastecu::logging::SsmLoggingProtocol> core_;
};
