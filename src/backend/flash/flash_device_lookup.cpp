#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{

int find_flash_device_index(std::string_view mcu_type)
{
    for (int i = 0; flashdevices[i].name != nullptr; ++i)
    {
        if (mcu_type == flashdevices[i].name)
        {
            return i;
        }
    }
    return -1;
}

const flashdev_t *find_flash_device(std::string_view mcu_type)
{
    const int index = find_flash_device_index(mcu_type);
    if (index < 0)
    {
        return nullptr;
    }
    return &flashdevices[index];
}

} // namespace fastecu::flash
