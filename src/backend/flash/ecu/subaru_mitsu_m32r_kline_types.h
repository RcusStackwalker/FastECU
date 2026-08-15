#pragma once
#include <cstdint>

#include "src/algorithms/protocol/bytes.h"

namespace fastecu::flash
{

struct SubaruMitsuM32rKlinePlan
{
    std::uint8_t tester_id;
    std::uint8_t target_id;
    int initial_baud;
    int flash_baud;
    std::uint32_t chunk_size;
    bytes::Byte unread_prefix_fill;
};

} // namespace fastecu::flash
