#pragma once

#include <optional>

#include "src/algorithms/protocol/bytes.h"

namespace fastecu::ui
{

struct ChecksumCorrectionResult
{
    std::optional<bytes::Bytes> corrected_rom_data;
    bool canceled_due_to_missing_module = false;
    bool unknown_mcu_type = false;
};

} // namespace fastecu::ui
