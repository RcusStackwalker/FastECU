#pragma once
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/checksum/checksum_selection.h"

namespace fastecu::checksum
{

// Pure, no I/O, no Qt. Replaces FileActions::checksum_correction's MCU/size
// lookup and flashMethod dispatch chain (file_actions.cpp:2153-2337).
ChecksumCorrectionOutcome apply_checksum_correction(bytes::ByteView rom_data, const ChecksumSelection& selection);

// Whether correction has a family for this make and flash method: the
// routing decision alone, without ROM bytes. The built-in catalog's checksum
// flags are tested against it.
bool has_route(std::string_view make, std::string_view flash_method);

} // namespace fastecu::checksum
