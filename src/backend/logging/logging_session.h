#pragma once

#include <string_view>
#include <vector>

#include "src/backend/logging/logging_types.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/backend/logging/logging_read_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

class LoggingSession
{
  public:
    LoggingProtocolId protocol() const;
    mutdma::FreeformDialect mut_dma_dialect() const;
    const std::optional<SsmReadPlan>& ssm_read_plan() const;
    const std::vector<LoggingChannel>& channels() const;
    const LoggingPolicy& policy() const;
    const LoggingChannel *find_channel(std::string_view id) const;

  private:
    LoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels, LoggingPolicy policy,
                   std::optional<SsmReadPlan> read_plan, mutdma::FreeformDialect dialect);

    LoggingProtocolId protocol_;
    std::vector<LoggingChannel> channels_;
    LoggingPolicy policy_;
    mutdma::FreeformDialect dialect_;
    std::optional<SsmReadPlan> read_plan_;

    friend fastecu::Result<LoggingSession> make_logging_session(LoggingProtocolId protocol,
                                                                std::vector<LoggingChannel> channels,
                                                                LoggingPolicy policy, mutdma::FreeformDialect dialect);
};

fastecu::Status validate_logging_channel(LoggingProtocolId protocol, const LoggingChannel& channel);

fastecu::Result<LoggingSession>
make_logging_session(LoggingProtocolId protocol, std::vector<LoggingChannel> channels, LoggingPolicy policy,
                     mutdma::FreeformDialect dialect = mutdma::FreeformDialect::LegacyBe);

} // namespace fastecu::logging
