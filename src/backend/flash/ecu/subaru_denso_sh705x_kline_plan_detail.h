#pragma once

#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/ports/result.h"

// Test seam: the static device tables all satisfy this check, so its failure
// branches are reachable only with a synthetic FlashDevice.
namespace fastecu::flash::detail
{
Status ValidateSubaruDensoSh705xKlineGeometry(const FlashDevice& device);
} // namespace fastecu::flash::detail
