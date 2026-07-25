#include "src/backend/checksum/flash_device_lookup.h"

namespace fastecu::checksum
{

const flashdev_t *find_flash_device(std::string_view mcu_type)
{
    for (int i = 0; flashdevices[i].name != nullptr; ++i)
    {
        if (mcu_type == flashdevices[i].name)
        {
            return &flashdevices[i];
        }
    }
    return nullptr;
}

} // namespace fastecu::checksum
