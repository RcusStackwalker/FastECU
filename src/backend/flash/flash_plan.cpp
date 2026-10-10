#include "src/backend/flash/flash_plan.h"

#include <optional>
#include <utility>

namespace fastecu::flash
{

const bytes::Bytes& FlashPlan::ImageOrEmpty() const
{
    static const bytes::Bytes kNoImage;
    return fields_.image.has_value() ? *fields_.image : kNoImage;
}

std::optional<memory::MemoryImage> FlashPlan::RomImage() const
{
    if (!fields_.image.has_value())
    {
        return std::nullopt;
    }
    std::optional<memory::MemoryMap> map = fields_.image_map;
    if (!map.has_value())
    {
        const std::optional<memory::ByteCount> size = memory::ByteCount::FromSize(fields_.image->size());
        if (!size.has_value())
        {
            return std::nullopt;
        }
        auto identity = memory::MemoryMap::Identity(*size);
        if (!identity.has_value())
        {
            return std::nullopt;
        }
        map = std::move(*identity);
    }
    auto image = memory::MemoryImage::Create(std::move(*map), *fields_.image);
    return image.has_value() ? std::optional(std::move(*image)) : std::nullopt;
}

const KernelImage& FlashPlan::KernelOrEmpty() const
{
    static const KernelImage kNoKernel{};
    return fields_.kernel.has_value() ? *fields_.kernel : kNoKernel;
}

std::string_view FlashPlan::ExperimentalFamilyId() const
{
    switch (fields_.family)
    {
    case FlashFamily::kDensoSh705xEepromKline:
        return "DensoSh705xEepromKline";
    case FlashFamily::kDensoSh705xEepromCan:
        return "DensoSh705xEepromCan";
    case FlashFamily::kMitsuColtM32rCan:
        return "MitsuColtM32rCan";
    case FlashFamily::kSubaruMitsuM32rKline:
        return "SubaruMitsuM32rKline";
    case FlashFamily::kSubaruHitachiM32rKline:
        return "SubaruHitachiM32rKline";
    case FlashFamily::kSubaruDensoMc68hc16y502:
        return "SubaruDensoMc68hc16y5_02";
    case FlashFamily::kSubaruDensoSh705502:
        return "SubaruDensoSh7055_02";
    case FlashFamily::kSubaruHitachiM32rCan:
        return "SubaruHitachiM32rCan";
    case FlashFamily::kSubaruTcuCvtHitachiM32rCan:
        return "SubaruTcuCvtHitachiM32rCan";
    case FlashFamily::kSubaruTcuCvtMitsuMh8111Can:
        return "SubaruTcuCvtMitsuMh8111Can";
    case FlashFamily::kSubaruTcuCvtMitsuMh8104Can:
        return "SubaruTcuCvtMitsuMh8104Can";
    case FlashFamily::kSubaruDenso1n83m15mCan:
        return "SubaruDenso1n83m_1_5mCan";
    case FlashFamily::kSubaruDensoSh72531Can:
        return "SubaruDensoSh72531Can";
    case FlashFamily::kSubaruDensoSh72543CanDiesel:
        return "SubaruDensoSh72543CanDiesel";
    case FlashFamily::kSubaruDenso1n83m4mCan:
        return "SubaruDenso1n83m_4mCan";
    case FlashFamily::kSubaruDensoSh705xDensoCan:
        return "SubaruDensoSh705xDensoCan";
    case FlashFamily::kSubaruTcuDensoSh705xCan:
        return "SubaruTcuDensoSh705xCan";
    case FlashFamily::kSubaruDensoSh7058Can:
        return "SubaruDensoSh7058Can";
    case FlashFamily::kSubaruDensoSh7058CanDiesel:
        return "SubaruDensoSh7058CanDiesel";
    case FlashFamily::kSubaruTcuHitachiM32rKline:
        return "SubaruTcuHitachiM32rKline";
    case FlashFamily::kSubaruHitachiSh72543rCan:
        return "SubaruHitachiSh72543rCan";
    case FlashFamily::kSubaruHitachiSh7058:
        return "SubaruHitachiSh7058";
    case FlashFamily::kSubaruTcuHitachiM32rCan:
        return "SubaruTcuHitachiM32rCan";
    case FlashFamily::kSubaruUnisiaJecs:
        return "SubaruUnisiaJecs";
    case FlashFamily::kSubaruDensoSh705xKline:
        return "SubaruDensoSh705xKline";
    case FlashFamily::kSubaruDensoMc68hc16y502Bdm:
        return "SubaruDensoMc68hc16y5_02Bdm";
    case FlashFamily::kSubaruUnisiaJecsM32rKline:
        return "SubaruUnisiaJecsM32rKline";
    case FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel:
        return "SubaruUnisiaJecsM32rBootModeKernel";
    case FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram:
        return "SubaruUnisiaJecsM32rBootModeProgram";
    }
    return "Unknown";
}

} // namespace fastecu::flash
