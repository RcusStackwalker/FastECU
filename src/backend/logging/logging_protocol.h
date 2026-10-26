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

// The precondition of every protocol call in a logging run: an operator's
// cancel wins over a closed adapter, and either stops the call before any
// wire traffic.
template <typename Transport>
fastecu::Status CheckLoggingRunCanContinue(const Transport& transport, const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return fastecu::Fail(fastecu::ErrorKind::kCancelled, "logging cancelled");
    }
    if (!transport.IsOpen())
    {
        return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "adapter disconnected");
    }
    return {};
}

class LoggingProtocol
{
  public:
    virtual ~LoggingProtocol() = default;
    virtual fastecu::Status Start(const fastecu::ICancellationToken&) = 0;
    virtual fastecu::Result<PollData> Poll(std::chrono::milliseconds timeout, const fastecu::ICancellationToken&) = 0;
    virtual fastecu::Status Stop() = 0;
};

} // namespace fastecu::logging
