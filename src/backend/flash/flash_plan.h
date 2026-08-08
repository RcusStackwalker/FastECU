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
    FlashOperation operation;
    FlashFamily family;
    TransportKind transport;
    std::string target_id;
    std::string mcu_name;
    MemoryRegion transfer_region;
    std::vector<MemoryRegion> erase_regions;
    std::optional<bytes::Bytes> image;
    // Optional because not every family uploads one. The EEPROM pair loads a
    // kernel file and uploads it; a future family may drive its own vendor
    // bootloader and upload only compile-time constants instead. See
    // family_requires_kernel_v (flash_types.h) for which families require one.
    std::optional<KernelImage> kernel;
    FamilyPlan family_plan;
    std::vector<ConfirmationSpec> confirmations;
};

class FlashPlan
{
  public:
    FlashOperation operation() const
    {
        return fields_.operation;
    }
    FlashFamily family() const
    {
        return fields_.family;
    }
    TransportKind transport() const
    {
        return fields_.transport;
    }
    const std::string& target_id() const
    {
        return fields_.target_id;
    }
    const std::string& mcu_name() const
    {
        return fields_.mcu_name;
    }
    const MemoryRegion& transfer_region() const
    {
        return fields_.transfer_region;
    }
    std::span<const MemoryRegion> erase_regions() const
    {
        return fields_.erase_regions;
    }
    const std::optional<bytes::Bytes>& image() const
    {
        return fields_.image;
    }
    const std::optional<KernelImage>& kernel() const
    {
        return fields_.kernel;
    }
    const FamilyPlan& family_plan() const
    {
        return fields_.family_plan;
    }
    std::span<const ConfirmationSpec> confirmations() const
    {
        return fields_.confirmations;
    }
    std::uint64_t total_transfer_bytes() const
    {
        return total_transfer_bytes_;
    }
    std::string_view experimental_family_id() const;

  private:
    friend Result<FlashPlan> validate_and_build(FlashPlanFields fields);

    explicit FlashPlan(FlashPlanFields fields, std::uint64_t total_transfer_bytes)
        : fields_(std::move(fields)), total_transfer_bytes_(total_transfer_bytes)
    {
    }

    FlashPlanFields fields_;
    std::uint64_t total_transfer_bytes_;
};

} // namespace fastecu::flash
