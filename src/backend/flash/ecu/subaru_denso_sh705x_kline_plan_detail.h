#pragma once

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/ports/result.h"

// Test seam: the static device tables all satisfy this check, so its failure
// branches are reachable only with a synthetic flashdev_t.
namespace fastecu::flash::detail
{
Status validate_subaru_denso_sh705x_kline_geometry(const flashdev_t& device);
} // namespace fastecu::flash::detail
