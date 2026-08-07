#pragma once

#include <string_view>

#include "src/backend/definitions/kernelmemorymodels.h"

namespace fastecu::flash
{

// Returns the flashdevices[] entry whose name matches mcu_type, or nullptr.
const flashdev_t *find_flash_device(std::string_view mcu_type);

// Returns the flashdevices[] index whose name matches mcu_type, or -1.
// Callers index the global table directly (flashdevices[i].fblocks[n].len
// and similar) throughout the legacy flash operations, so the index form is
// load-bearing and not merely a convenience over the pointer form.
int find_flash_device_index(std::string_view mcu_type);

} // namespace fastecu::flash
