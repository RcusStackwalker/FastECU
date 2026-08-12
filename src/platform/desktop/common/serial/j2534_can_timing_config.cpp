#include "src/platform/desktop/common/serial/j2534_can_timing_config.h"

#include <array>

bool configureJ2534CanTimings(bool iso15765, const J2534SetConfig& setConfig)
{
    std::array<SCONFIG, 3> config{{
        {LOOPBACK, 0},
        {ISO15765_STMIN, 0},
        {ISO15765_BS, 0},
    }};
    SCONFIG_LIST list{
        static_cast<unsigned long>(iso15765 ? config.size() : std::size_t{1}),
        config.data(),
    };
    if (setConfig(list) == STATUS_NOERROR)
    {
        return true;
    }
    if (!iso15765)
    {
        return false;
    }

    config[1].Value = 1;
    config[2].Value = 16;
    return setConfig(list) == STATUS_NOERROR;
}
