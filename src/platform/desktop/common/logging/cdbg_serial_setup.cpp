#include "src/platform/desktop/common/logging/cdbg_serial_setup.h"

#include <array>
#include <string_view>

namespace fastecu::desktop::logging
{

fastecu::Status configure_cdbg_serial(const CdbgSerialSetupActions& actions)
{
    const std::array<std::pair<std::string_view, const std::function<bool()> *>, 7> steps{{
        {"disable ISO 14230 mode", &actions.disable_iso14230},
        {"disable ISO 14230 header", &actions.disable_iso14230_header},
        {"enable raw CAN mode", &actions.enable_raw_can},
        {"disable ISO 15765 mode", &actions.disable_iso15765},
        {"select 11-bit CAN identifiers", &actions.select_11_bit_ids},
        {"select 500000 baud", &actions.select_500k_baud},
        {"select CDBG reply identifier", &actions.select_reply_id},
    }};
    for (const auto& [description, action] : steps)
    {
        if (!*action || !(*action)())
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                                 "failed to " + std::string(description));
        }
    }
    return {};
}

} // namespace fastecu::desktop::logging
