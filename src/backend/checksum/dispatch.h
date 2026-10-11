#pragma once
#include <string_view>

#include "src/algorithms/memory/memory_image.h"
#include "src/backend/checksum/checksum_selection.h"

namespace fastecu::checksum
{

// Pure, no I/O, no Qt. Replaces FileActions::checksum_correction's MCU/size
// lookup and flashMethod dispatch chain (file_actions.cpp:2153-2337).
//
// The flash method's route declares the ECU window its family checksums; the
// image's memory map must span exactly that window, with a block holding every
// address. The family runs on the window's bytes, and the bytes it corrects go
// back into a copy of the ROM file through the memory map's write rules.
ChecksumCorrectionOutcome ApplyChecksumCorrection(const memory::MemoryImage& image, const ChecksumSelection& selection);

// Whether correction has a family for this make and flash method: the
// routing decision alone, without ROM bytes. The built-in catalog's checksum
// flags are tested against it.
bool HasRoute(std::string_view make, std::string_view flash_method);

} // namespace fastecu::checksum
