#pragma once
#include <memory>
#include "logging/logging_protocol.h"
#include "protocol/ican_transport.h"
#include "protocol/mitsu_colt_can_cdbg_driver.h"
#include <file_actions.h>

class SerialPortActions;

class CdbgLoggingProtocol : public LoggingProtocol
{
  public:
    CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport, SerialPortActions *serial,
                        FileActions::LogValuesStructure *logValues, FileActions *fileActions);

    bool start(QString *errorOut) override;
    PollResult poll(int timeoutMs) override;
    void stop() override;

  private:
    std::unique_ptr<cdbg::ICanTransport> transport_;
    SerialPortActions *serial_;
    FileActions::LogValuesStructure *logValues_;
    FileActions *fileActions_;
    MitsuColtCanCdbg::CdbgLogDriver driver_;
    QVector<int> channelLogValueIndex_;
};
