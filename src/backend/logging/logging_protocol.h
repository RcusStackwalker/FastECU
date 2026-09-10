#pragma once

#include <chrono>
#include <vector>

#include "src/backend/logging/logging_types.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

struct PollData
{
    bool responded = false;
    std::vector<ProtocolSample> samples;
};

class LoggingProtocol
{
  public:
    virtual ~LoggingProtocol() = default;
    virtual fastecu::Status start(const fastecu::ICancellationToken&) = 0;
    virtual fastecu::Result<PollData> poll(std::chrono::milliseconds timeout, const fastecu::ICancellationToken&) = 0;
    virtual fastecu::Status stop() = 0;
};

} // namespace fastecu::logging
