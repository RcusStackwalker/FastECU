#include "src/backend/logging/logging_protocol.h"
#include "src/backend/logging/portable_logging_protocol.h"

#include <concepts>

static_assert(!std::same_as<::LogSample, fastecu::logging::LogSample>);
static_assert(!std::same_as<::PollData, fastecu::logging::PollData>);
