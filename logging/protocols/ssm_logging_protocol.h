#pragma once
#include <memory>
#include <QString>
#include "logging/logging_protocol.h"
#include "protocol/bytes.h"
#include "protocol/issm_transport.h"
#include <file_actions.h>

class SsmLoggingProtocol : public LoggingProtocol {
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
