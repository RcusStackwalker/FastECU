#pragma once

#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/ports/result.h"

// Test seam: the static device tables all satisfy this check, so its failure
// branches are reachable only with a synthetic FlashDevice.
namespace fastecu::flash::detail
{
Status validate_subaru_denso_sh705x_kline_geometry(const FlashDevice& device);
} // namespace fastecu::flash::detail
