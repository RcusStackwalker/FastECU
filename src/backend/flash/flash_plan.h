#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// Unvalidated inputs a builder assembles before handing them to
// validate_and_build (Task 3). Every field here is copied by value; nothing
// in FlashPlan borrows from its caller after construction.
struct FlashPlanFields
{
    FlashOperation operation{};
    FlashFamily family{};
    TransportKind transport{};
    std::string target_id;
    std::string mcu_name;
    MemoryRegion transfer_region{};
    std::vector<MemoryRegion> erase_regions;
    std::optional<bytes::Bytes> image;
    // Optional because not every family uploads one. The EEPROM pair loads a
    // kernel file and uploads it; the Mitsu Colt CAN family drives the ECU's
    // own vendor bootloader and uploads only compile-time RAM helper routines
    // that are protocol constants, not a loaded image. See
    // kFamilyRequiresKernel (flash_types.h) for which families require one.
    std::optional<KernelImage> kernel;
    FamilyPlan family_plan;
    std::vector<ConfirmationSpec> confirmations;
};

class FlashPlan
{
  public:
    FlashOperation Operation() const
    {
        return fields_.operation;
    }
    FlashFamily Family() const
    {
        return fields_.family;
    }
    TransportKind Transport() const
    {
        return fields_.transport;
    }
    const std::string& TargetId() const
    {
        return fields_.target_id;
    }
    const std::string& McuName() const
    {
        return fields_.mcu_name;
    }
    const MemoryRegion& TransferRegion() const
    {
        return fields_.transfer_region;
    }
    std::span<const MemoryRegion> EraseRegions() const
    {
        return fields_.erase_regions;
    }
    const std::optional<bytes::Bytes>& Image() const
    {
        return fields_.image;
    }
    const std::optional<KernelImage>& Kernel() const
    {
        return fields_.kernel;
    }
    // The image of a Write/TestWrite plan, which validate_and_build requires
    // to carry one. Empty for a Read plan, so a caller that skips the
    // operation check reads zero bytes (which every writer rejects by length)
    // instead of dereferencing an empty optional.
    const bytes::Bytes& ImageOrEmpty() const;
    // The kernel of a family that requires one (kFamilyRequiresKernel).
    // Empty for a plan without one, for the same reason as image_or_empty().
    const KernelImage& KernelOrEmpty() const;
    const FamilyPlan& FamilyPlan() const
    {
        return fields_.family_plan;
    }
    std::span<const ConfirmationSpec> Confirmations() const
    {
        return fields_.confirmations;
    }
    std::uint64_t TotalTransferBytes() const
    {
        return total_transfer_bytes_;
    }
    std::string_view ExperimentalFamilyId() const;

  private:
    friend Result<FlashPlan> ValidateAndBuild(FlashPlanFields fields);

    explicit FlashPlan(FlashPlanFields fields, std::uint64_t total_transfer_bytes)
        : fields_(std::move(fields)), total_transfer_bytes_(total_transfer_bytes)
    {
    }

    FlashPlanFields fields_;
    std::uint64_t total_transfer_bytes_;
};

} // namespace fastecu::flash
