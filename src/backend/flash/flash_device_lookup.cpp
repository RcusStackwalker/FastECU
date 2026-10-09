#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{

int find_flash_device_index(std::string_view mcu_type)
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

const FlashDevice *find_flash_device(std::string_view mcu_type)
{
    const int index = find_flash_device_index(mcu_type);
    if (index < 0)
    {
        return nullptr;
    }
    return &kFlashDevices[index];
}

} // namespace fastecu::flash
