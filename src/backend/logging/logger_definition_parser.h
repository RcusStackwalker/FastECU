#pragma once

#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/logging/logger_definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

// Parses a RomRaider-style logger definition document. Pure: no file I/O and
// no configuration lookup -- the caller supplies the bytes and a `source`
// label used only to build error messages.
//
// Missing optional attributes fall back to the legacy placeholder strings
// ("No id", "No desc", "#", ...) that downstream comparisons still rely on.
// Rows are always aligned: a <parameter> without <address> or <conversions>
// yields empty fields, never a short vector.
Result<LoggerDefinition> parse_logger_definition(bytes::ByteView xml, std::string_view source);

} // namespace fastecu::logging
