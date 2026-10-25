#pragma once

#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash::detail
{
bool FixedShapeDeviceMatches(const FlashDevice *device, std::uint32_t rom_size, unsigned block_count);
bool OperationImageShapeMatches(const FlashPlan& plan, std::uint32_t rom_size);
// Callers validate the flash table before comparing transfer geometry.
bool TransferMatchesDevice(const MemoryRegion& transfer, const FlashDevice& device);
// True when the plan declares exactly the confirmation `id`, without arguments.
bool RequiresOnly(std::span<const ConfirmationSpec> confirmations, ConfirmationSpec::Id id);

// Expected addresses and block sizes come from each family's protocol.
// Use a numeric size so overflow boundaries can be checked without allocating
// an image. Address and padding arithmetic use 64 bits to avoid wrapping.
// Callers validate the device table and reject empty kernels.
template <std::uint64_t BlockSize>
Status ValidateKernelUpload(std::uint64_t size, std::uint32_t load_address, std::uint32_t expected_load_address,
                            const KernelBlock& region)
{
    static_assert(BlockSize > 0);
    if (load_address != expected_load_address)
    {
        return Fail(ErrorKind::kInvalidConfig, "kernel address does not match the selected protocol");
    }
    constexpr std::uint64_t kPadding = BlockSize - 1;
    if (size > std::numeric_limits<std::uint64_t>::max() - kPadding)
    {
        return Fail(ErrorKind::kInvalidConfig, "kernel size cannot be padded to transfer blocks");
    }
    const std::uint64_t padded_size = ((size + kPadding) / BlockSize) * BlockSize;
    const std::uint64_t region_start = region.start;
    const std::uint64_t region_end = region_start + region.len;
    const std::uint64_t upload_start = load_address;
    if (upload_start < region_start || upload_start > region_end || padded_size > region_end - upload_start)
    {
        return Fail(ErrorKind::kInvalidConfig, "padded kernel lies outside the MCU kernel region");
    }
    return {};
}

// Callers validate the device table before comparing the block count and
// ordered addresses and lengths.
Status ValidateRegions(const FlashPlan& plan, const FlashDevice& device);
bool EraseGeometryMatches(std::span<const MemoryRegion> regions, const FlashDevice& device);
std::vector<MemoryRegion> MakeEraseRegions(const FlashDevice& device);

// The plan fields a kernel-loaded whole-ROM family's builder shares, for a
// device the caller has already validated: the kernel must fit the device's
// kernel region from `kernel_load_address`, a read carries no image, any
// other operation carries exactly one whole-ROM image and erases every flash
// block, and the transfer covers the whole ROM. The caller adds its family,
// transport, wire parameters and confirmations.
template <std::uint64_t KernelBlockSize>
Result<FlashPlanFields> WholeRomKernelPlanFields(FlashOperation operation, std::string_view protocol_name,
                                                 std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                 KernelImage kernel, std::uint32_t kernel_load_address,
                                                 const FlashDevice& device)
{
    if (Status upload = ValidateKernelUpload<KernelBlockSize>(kernel.bytes.size(), kernel.load_address,
                                                              kernel_load_address, device.kblocks[0]);
        !upload.has_value())
    {
        return std::unexpected(upload.error());
    }
    const bool reading = operation == FlashOperation::kRead;
    if (reading && image.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "read plans must not carry an image");
    }
    if (!reading && (!image.has_value() || image->size() != device.romsize))
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device.romsize));
    }
    return FlashPlanFields{
        .operation = operation,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = {device.fblocks[0].start, device.romsize},
        .erase_regions = reading ? std::vector<MemoryRegion>{} : MakeEraseRegions(device),
        .image = reading ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
    };
}
} // namespace fastecu::flash::detail
