#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/checksum/checksum_selection.h"

namespace fastecu::checksum
{

// Pure, no I/O, no Qt. Replaces FileActions::checksum_correction's MCU/size
// lookup and flashMethod dispatch chain (file_actions.cpp:2153-2337).
ChecksumCorrectionOutcome apply_checksum_correction(bytes::ByteView rom_data, const ChecksumSelection& selection);

} // namespace fastecu::checksum
