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
    switch (family)
    {
    case FlashFamily::kDensoSh705xEepromKline:
        *os << "DensoSh705xEepromKline";
        return;
    case FlashFamily::kDensoSh705xEepromCan:
        *os << "DensoSh705xEepromCan";
        return;
    case FlashFamily::kMitsuColtM32rCan:
        *os << "MitsuColtM32rCan";
        return;
    case FlashFamily::kSubaruMitsuM32rKline:
        *os << "SubaruMitsuM32rKline";
        return;
    case FlashFamily::kSubaruHitachiM32rKline:
        *os << "SubaruHitachiM32rKline";
        return;
    case FlashFamily::kSubaruDensoMc68hc16y502:
        *os << "SubaruDensoMc68hc16y5_02";
        return;
    case FlashFamily::kSubaruDensoSh705502:
        *os << "SubaruDensoSh7055_02";
        return;
    case FlashFamily::kSubaruHitachiM32rCan:
        *os << "SubaruHitachiM32rCan";
        return;
    case FlashFamily::kSubaruTcuCvtHitachiM32rCan:
        *os << "SubaruTcuCvtHitachiM32rCan";
        return;
    case FlashFamily::kSubaruTcuCvtMitsuMh8111Can:
        *os << "SubaruTcuCvtMitsuMh8111Can";
        return;
    case FlashFamily::kSubaruTcuCvtMitsuMh8104Can:
        *os << "SubaruTcuCvtMitsuMh8104Can";
        return;
    case FlashFamily::kSubaruDenso1n83m15mCan:
        *os << "SubaruDenso1n83m_1_5mCan";
        return;
    case FlashFamily::kSubaruDensoSh72531Can:
        *os << "SubaruDensoSh72531Can";
        return;
    case FlashFamily::kSubaruDensoSh72543CanDiesel:
        *os << "SubaruDensoSh72543CanDiesel";
        return;
    case FlashFamily::kSubaruDenso1n83m4mCan:
        *os << "SubaruDenso1n83m_4mCan";
        return;
    case FlashFamily::kSubaruDensoSh705xDensoCan:
        *os << "SubaruDensoSh705xDensoCan";
        return;
    case FlashFamily::kSubaruTcuDensoSh705xCan:
        *os << "SubaruTcuDensoSh705xCan";
        return;
    case FlashFamily::kSubaruDensoSh7058Can:
        *os << "SubaruDensoSh7058Can";
        return;
    case FlashFamily::kSubaruDensoSh7058CanDiesel:
        *os << "SubaruDensoSh7058CanDiesel";
        return;
    case FlashFamily::kSubaruTcuHitachiM32rKline:
        *os << "SubaruTcuHitachiM32rKline";
        return;
    case FlashFamily::kSubaruHitachiSh72543rCan:
        *os << "SubaruHitachiSh72543rCan";
        return;
    case FlashFamily::kSubaruHitachiSh7058:
        *os << "SubaruHitachiSh7058";
        return;
    case FlashFamily::kSubaruTcuHitachiM32rCan:
        *os << "SubaruTcuHitachiM32rCan";
        return;
    case FlashFamily::kSubaruUnisiaJecs:
        *os << "SubaruUnisiaJecs";
        return;
    case FlashFamily::kSubaruDensoSh705xKline:
        *os << "SubaruDensoSh705xKline";
        return;
    case FlashFamily::kSubaruDensoMc68hc16y502Bdm:
        *os << "SubaruDensoMc68hc16y5_02Bdm";
        return;
    case FlashFamily::kSubaruUnisiaJecsM32rKline:
        *os << "SubaruUnisiaJecsM32rKline";
        return;
    case FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel:
        *os << "SubaruUnisiaJecsM32rBootModeKernel";
        return;
    case FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram:
        *os << "SubaruUnisiaJecsM32rBootModeProgram";
        return;
    }
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
