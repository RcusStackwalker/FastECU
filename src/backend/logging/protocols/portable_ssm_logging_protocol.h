#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/logging/portable_logging_protocol.h"
#include "src/backend/ports/clock.h"
#include "src/backend/protocol/issm_transport.h"

namespace fastecu::logging
{

class SsmLoggingProtocol final : public LoggingProtocol
{
  public:
    SsmLoggingProtocol(fastecu::IClock& clock,
                       std::unique_ptr<ISsmTransport> transport,
                       std::vector<LoggingChannel> channels,
                       bool target_is_ecu, bool use_openport2_adapter);
    SsmLoggingProtocol(fastecu::IClock& clock,
                       std::unique_ptr<ISsmTransport> transport,
                       std::vector<LoggingChannel> channels,
                       std::vector<std::size_t> response_offsets,
                       bool target_is_ecu, bool use_openport2_adapter);

    fastecu::Status start(const fastecu::ICancellationToken& cancellation) override;
    fastecu::Result<PollData> poll(
        int timeout_ms, const fastecu::ICancellationToken& cancellation) override;
    fastecu::Status stop() override;

  private:
    bytes::Bytes buildSsmHeader(bytes::ByteView output) const;
    fastecu::Result<bytes::Bytes> readFramedResponse(
        int timeout_ms, const fastecu::ICancellationToken& cancellation);

    fastecu::IClock& clock_;
    std::unique_ptr<ISsmTransport> transport_;
    const std::vector<LoggingChannel> channels_;
    const std::vector<std::size_t> response_offsets_;
    const bool target_is_ecu_;
    const bool use_openport2_adapter_;
};

} // namespace fastecu::logging
