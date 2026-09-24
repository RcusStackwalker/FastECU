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
    case FlashOperation::Read:
        *os << "Read";
        return;
    case FlashOperation::Write:
        *os << "Write";
        return;
    case FlashOperation::TestWrite:
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
    case FlashFamily::DensoSh705xEepromKline:
        *os << "DensoSh705xEepromKline";
        return;
    case FlashFamily::DensoSh705xEepromCan:
        *os << "DensoSh705xEepromCan";
        return;
    case FlashFamily::MitsuColtM32rCan:
        *os << "MitsuColtM32rCan";
        return;
    case FlashFamily::SubaruMitsuM32rKline:
        *os << "SubaruMitsuM32rKline";
        return;
    case FlashFamily::SubaruHitachiM32rKline:
        *os << "SubaruHitachiM32rKline";
        return;
    case FlashFamily::SubaruDensoMc68hc16y5_02:
        *os << "SubaruDensoMc68hc16y5_02";
        return;
    case FlashFamily::SubaruDensoSh7055_02:
        *os << "SubaruDensoSh7055_02";
        return;
    case FlashFamily::SubaruHitachiM32rCan:
        *os << "SubaruHitachiM32rCan";
        return;
    case FlashFamily::SubaruTcuCvtHitachiM32rCan:
        *os << "SubaruTcuCvtHitachiM32rCan";
        return;
    case FlashFamily::SubaruTcuCvtMitsuMh8111Can:
        *os << "SubaruTcuCvtMitsuMh8111Can";
        return;
    case FlashFamily::SubaruTcuCvtMitsuMh8104Can:
        *os << "SubaruTcuCvtMitsuMh8104Can";
        return;
    case FlashFamily::SubaruDenso1n83m_1_5mCan:
        *os << "SubaruDenso1n83m_1_5mCan";
        return;
    case FlashFamily::SubaruDensoSh72531Can:
        *os << "SubaruDensoSh72531Can";
        return;
    case FlashFamily::SubaruDensoSh72543CanDiesel:
        *os << "SubaruDensoSh72543CanDiesel";
        return;
    case FlashFamily::SubaruDenso1n83m_4mCan:
        *os << "SubaruDenso1n83m_4mCan";
        return;
    case FlashFamily::SubaruDensoSh705xDensoCan:
        *os << "SubaruDensoSh705xDensoCan";
        return;
    case FlashFamily::SubaruTcuDensoSh705xCan:
        *os << "SubaruTcuDensoSh705xCan";
        return;
    case FlashFamily::SubaruDensoSh7058Can:
        *os << "SubaruDensoSh7058Can";
        return;
    case FlashFamily::SubaruDensoSh7058CanDiesel:
        *os << "SubaruDensoSh7058CanDiesel";
        return;
    case FlashFamily::SubaruTcuHitachiM32rKline:
        *os << "SubaruTcuHitachiM32rKline";
        return;
    case FlashFamily::SubaruHitachiSh72543rCan:
        *os << "SubaruHitachiSh72543rCan";
        return;
    case FlashFamily::SubaruHitachiSh7058:
        *os << "SubaruHitachiSh7058";
        return;
    case FlashFamily::SubaruTcuHitachiM32rCan:
        *os << "SubaruTcuHitachiM32rCan";
        return;
    case FlashFamily::SubaruUnisiaJecs:
        *os << "SubaruUnisiaJecs";
        return;
    case FlashFamily::SubaruDensoSh705xKline:
        *os << "SubaruDensoSh705xKline";
        return;
    }
}

inline void PrintTo(TransportKind transport, std::ostream *os)
{
    switch (transport)
    {
    case TransportKind::Kline:
        *os << "Kline";
        return;
    case TransportKind::CanIso15765:
        *os << "CanIso15765";
        return;
    case TransportKind::CanRawIso15765:
        *os << "CanRawIso15765";
        return;
    }
}

} // namespace fastecu::flash
