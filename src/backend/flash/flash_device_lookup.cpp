#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{

int FindFlashDeviceIndex(std::string_view mcu_type)
{
    for (int i = 0; kFlashDevices[i].name != nullptr; ++i)
    {
        if (mcu_type == kFlashDevices[i].name)
        {
            return i;
        }
    }
    return -1;
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
