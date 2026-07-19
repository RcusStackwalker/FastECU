#pragma once
#include <memory>
#include <QString>
#include "src/backend/logging/logging_protocol.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/protocol/issm_transport.h"
#include "src/backend/definitions/file_actions.h"

class SsmLoggingProtocol : public LoggingProtocol
{
  public:
    SsmLoggingProtocol(std::unique_ptr<ISsmTransport> transport,
                       FileActions::LogValuesStructure *logValues, FileActions *fileActions,
                       QString logValueProtocolFilter, bool targetIsEcu, bool useOpenport2Adapter);

    bool start(QString *errorOut) override;
    PollResult poll(int timeoutMs) override;
    void stop() override;

  private:
    bytes::Bytes buildSsmHeader(bytes::ByteView output) const;
    bytes::Bytes readFramedResponse(int timeoutMs);

    std::unique_ptr<ISsmTransport> transport_;
    FileActions::LogValuesStructure *logValues_;
    FileActions *fileActions_;
    QString logValueProtocolFilter_;
    bool targetIsEcu_;
    bool useOpenport2Adapter_;
};
