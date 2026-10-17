#pragma once

#include <ostream>

#include <format>

#include "src/backend/flash/flash_types.h"

namespace fastecu::flash
{

inline void PrintTo(FlashOperation operation, std::ostream *os)
{
    switch (operation)
    {
    case FlashOperation::kRead:
        *os << "Read";
        return;
    case FlashOperation::kWrite:
        *os << "Write";
        return;
    case FlashOperation::kTestWrite:
        *os << "TestWrite";
        return;
    }
}

inline void PrintTo(const MemoryRegion& region, std::ostream *os)
{
    *os << std::format("[0x{:x}, len 0x{:x})", region.start, region.length);
}

inline void PrintTo(FlashFamily family, std::ostream *os)
{
    *os << FlashFamilyName(family);
}

inline void PrintTo(TransportKind transport, std::ostream *os)
{
    switch (transport)
    {
    case TransportKind::kKline:
        *os << "Kline";
        return;
    case TransportKind::kCanIso15765:
        *os << "CanIso15765";
        return;
    case TransportKind::kCanRawIso15765:
        *os << "CanRawIso15765";
        return;
    }
}

} // namespace fastecu::flash
