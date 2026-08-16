#pragma once

#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/logging/logger_definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

// The three operations the legacy read_logger_conf fused behind its `modify`
// flag, split apart. All pure: the caller supplies and stores the bytes.

// Returns nullopt when `ecu_id` has no <ecu> element -- it does not
// initialize anything. Composing a default and writing it is the service's
// job, so the write is an explicit step rather than a side effect of a read.
Result<std::optional<LoggerSelection>> read_selection(bytes::ByteView conf, std::string_view ecu_id,
                                                      std::string_view source);

// Updates `ecu_id`'s <ecu> element in place, or appends one if absent, and
// returns the whole re-serialized document. Four-space indented to match what
// QDomDocument::save(output, 4) wrote, so an existing conf file does not
// reflow wholesale on first write.
Result<bytes::Bytes> write_selection(bytes::ByteView conf, std::string_view ecu_id, const LoggerSelection& selection,
                                     std::string_view source);

// The first-N walk read_logger_definition_file performs at parse time:
// 15 gauges / 12 lower-panel / 20 switches, ignoring `enabled`.
LoggerSelection initial_selection(const LoggerDefinition& definition);

// The enabled-only walk read_logger_conf performs when the ECU id is absent,
// at the same caps.
LoggerSelection default_selection(const LoggerDefinition& definition);

} // namespace fastecu::logging
