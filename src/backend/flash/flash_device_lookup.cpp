#include "src/backend/flash/flash_device_lookup.h"

#include <algorithm>
#include <ranges>

namespace fastecu::flash
{

int FindFlashDeviceIndex(std::string_view mcu_type)
{
    const auto found = std::ranges::find(kFlashDevices, mcu_type, &FlashDevice::name);
    return found == kFlashDevices.end() ? -1 : static_cast<int>(std::ranges::distance(kFlashDevices.begin(), found));
}

const FlashDevice *FindFlashDevice(std::string_view mcu_type)
{
    const int index = FindFlashDeviceIndex(mcu_type);
    if (index < 0)
    {
        return nullptr;
    }
    return &kFlashDevices[index];
}

} // namespace fastecu::flash
