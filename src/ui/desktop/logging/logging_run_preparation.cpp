#include "src/ui/desktop/logging/logging_run_preparation.h"

#include <chrono>
#include <expected>
#include <utility>

namespace fastecu::ui
{
Result<PreparedLoggingRun> prepareLoggingRun(const logging::LoggerModel& model, LoggingRunChoices choices)
{
    using namespace std::chrono_literals;

    std::string registrationKey = "SSM";
    auto protocol = logging::LoggingProtocolId::kSsm;
    auto definitionFilter = choices.ssm_definition_filter;
    logging::LoggingPolicy policy{.poll_timeout = 300ms,
                                  .car_silence_miss_threshold = 10,
                                  .reconnect_attempt_threshold = 30,
                                  .reconnect_retry_period = 10};
    if (choices.saved_protocol == "MUT_DMA" || choices.saved_protocol == "CDBG")
    {
        registrationKey = choices.saved_protocol;
        protocol = choices.saved_protocol == "MUT_DMA" ? logging::LoggingProtocolId::kMutDma
                                                       : logging::LoggingProtocolId::kCdbg;
        definitionFilter = choices.saved_protocol;
        policy = {.poll_timeout = 50ms,
                  .car_silence_miss_threshold = 20,
                  .reconnect_attempt_threshold = 100,
                  .reconnect_retry_period = 20};
    }

    auto snapshot = logging::PrepareLoggingRun(model, protocol, definitionFilter, policy, choices.target);
    if (!snapshot.has_value())
    {
        return std::unexpected(snapshot.error());
    }
    return PreparedLoggingRun{.registration_key = std::move(registrationKey), .snapshot = std::move(*snapshot)};
}
} // namespace fastecu::ui
