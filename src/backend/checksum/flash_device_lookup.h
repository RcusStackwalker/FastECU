#pragma once
#include <string_view>
#include "src/backend/definitions/kernelmemorymodels.h"

namespace fastecu::checksum
{

// Portable equivalent of FlashUtils::findFlashDeviceIndex
// (src/backend/flash/flash_utils.cpp, Qt-typed), scoped to this package
// rather than changing that function or its other Qt-linked callers.
const flashdev_t *find_flash_device(std::string_view mcu_type);

} // namespace fastecu::checksum
