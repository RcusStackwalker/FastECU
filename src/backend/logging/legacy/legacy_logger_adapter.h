#pragma once

#include "src/backend/definitions/log_values.h"
#include "src/backend/logging/logger_definition_model.h"

namespace fastecu::logging
{

// Fans the portable logger types out into the legacy parallel-array struct.
//
// apply_definition must run EXACTLY ONCE, at load. log_value_enabled and
// log_switch_enabled hold the XML defaults only until log_operations_ssm
// overwrites them from the ECU's capability response; re-applying a
// definition afterwards would silently reset every reported capability.
void apply_definition(const LoggerDefinition& definition, definitions::LogValuesStructure& values);

// Writes only the four selection fields. Never touches definition state.
void apply_selection(const LoggerSelection& selection, definitions::LogValuesStructure& values);

} // namespace fastecu::logging
