#pragma once
#include <memory>
#include "src/backend/logging/logging_protocol.h"
#include "src/backend/logging/protocols/portable_cdbg_logging_protocol.h"
#include "src/backend/definitions/file_actions.h"

class SerialPortActions;

class CdbgLoggingProtocol : public LoggingProtocol
{
  public:
    CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport, SerialPortActions *serial,
                        FileActions::LogValuesStructure *logValues, FileActions *fileActions);

    fastecu::Status start() override;
    fastecu::Result<PollData> poll(int timeoutMs) override;
    void stop() override;

  private:
    FileActions::LogValuesStructure *logValues_;
    FileActions *fileActions_;
    QVector<int> channelLogValueIndex_;
    std::unique_ptr<fastecu::logging::CdbgLoggingProtocol> core_;
};
