#pragma once

#include <string_view>
#include <vector>

#include "src/algorithms/expression/expression.h"
#include "src/backend/logging/logging_types.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/backend/logging/logging_read_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

class LoggingSession
{
  public:
    LoggingProtocolId Protocol() const;
    mutdma::FreeformDialect MutDmaDialect() const;
    const std::optional<SsmReadPlan>& ReadPlan() const;
    const std::vector<LoggingChannel>& Channels() const;
    const LoggingPolicy& Policy() const;
    const LoggingChannel *FindChannel(std::string_view id) const;
    const expression::CompiledExpression *FindConversion(std::string_view id) const;

  private:
    LoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels, LoggingPolicy policy,
                   std::optional<SsmReadPlan> read_plan, mutdma::FreeformDialect dialect, std::vector<expression::CompiledExpression> conversions);

    LoggingProtocolId protocol_;
    std::vector<LoggingChannel> channels_;
    LoggingPolicy policy_;
    mutdma::FreeformDialect dialect_;
    std::optional<SsmReadPlan> read_plan_;
    std::vector<expression::CompiledExpression> conversions_;

    friend fastecu::Result<LoggingSession> MakeLoggingSession(LoggingProtocolId protocol,
                                                              std::vector<LoggingChannel> channels,
                                                              LoggingPolicy policy, mutdma::FreeformDialect dialect);
};

fastecu::Status ValidateLoggingChannel(LoggingProtocolId protocol, const LoggingChannel& channel);

fastecu::Result<LoggingSession>
MakeLoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels, LoggingPolicy policy,
                   mutdma::FreeformDialect dialect = mutdma::FreeformDialect::kLegacyBe);

} // namespace fastecu::logging
