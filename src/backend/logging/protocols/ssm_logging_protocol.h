#pragma once
#include <memory>
#include <QString>
#include "src/backend/logging/logging_protocol.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/protocol/issm_transport.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/clock.h"

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
    bytes::Bytes buildSsmHeader(bytes::ByteView output) const;
    bytes::Bytes readFramedResponse(int timeoutMs);

    fastecu::IClock& clock_;
    std::unique_ptr<ISsmTransport> transport_;
    FileActions::LogValuesStructure *logValues_;
    FileActions *fileActions_;
    QString logValueProtocolFilter_;
    bool targetIsEcu_;
    bool useOpenport2Adapter_;
};
