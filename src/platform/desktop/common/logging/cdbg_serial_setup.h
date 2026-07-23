#pragma once

#include <functional>

#include "src/backend/ports/result.h"

namespace fastecu::desktop::logging
{

struct CdbgSerialSetupActions
{
    std::function<bool()> disable_iso14230;
    std::function<bool()> disable_iso14230_header;
    std::function<bool()> enable_raw_can;
    std::function<bool()> disable_iso15765;
    std::function<bool()> select_11_bit_ids;
    std::function<bool()> select_500k_baud;
    std::function<bool()> select_reply_id;
};

fastecu::Status configure_cdbg_serial(const CdbgSerialSetupActions& actions);

} // namespace fastecu::desktop::logging
