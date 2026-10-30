#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
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
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash::detail
{
bool FixedShapeDeviceMatches(const FlashDevice *device, std::uint32_t rom_size, unsigned block_count);
bool OperationImageShapeMatches(const FlashPlan& plan, std::uint32_t rom_size);
// Projects the protocol-id column of a family table, in table order.
template <auto Member, typename Entry, std::size_t N>
consteval std::array<std::string_view, N> ProtocolIdsOf(const std::array<Entry, N>& table)
{
    std::array<std::string_view, N> ids{};
    for (std::size_t i = 0; i < N; ++i)
    {
        ids[i] = table[i].*Member;
    }
    return ids;
}
// Looks `protocol` up in a family's catalog of exact protocol ids, whose rows
// carry `protocol` and `mcu`: an unknown protocol or another MCU is
// kInvalidConfig, worded with the family's `name`.
template <typename Entry, std::size_t N>
Result<Entry> FindCatalogEntry(const std::array<Entry, N>& catalog, std::string_view name, std::string_view protocol,
                               std::string_view mcu)
{
    const auto it = std::ranges::find(catalog, protocol, &Entry::protocol);
    if (it == catalog.end())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("Unsupported {} protocol: {}", name, protocol));
    }
    if (mcu != it->mcu)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("{} protocol {} requires MCU {}, not {}", name, protocol, it->mcu, mcu));
    }
    return *it;
}

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

// Checks the parts of a kernel-loaded whole-ROM family's plan that
// BuildWholeRomKernelPlan derives, on a device the caller has already
// validated: a kernel that fits the device's kernel region from
// `kernel_load_address`, the device's erase regions, and the operation's
// image shape.
template <std::uint64_t KernelBlockSize>
Status ValidateWholeRomKernelPlan(const FlashPlan& plan, std::uint32_t kernel_load_address, const FlashDevice& device)
{
    if (!plan.Kernel().has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "plan requires a kernel image");
    }
    return ValidateKernelUpload<KernelBlockSize>(plan.Kernel()->bytes.size(), plan.Kernel()->load_address,
                                                 kernel_load_address, device.kblocks[0])
        .and_then([&] { return ValidateRegions(plan, device); })
        .and_then(
            [&]() -> Status
            {
                if (!OperationImageShapeMatches(plan, device.romsize))
                {
                    return Fail(ErrorKind::kInvalidConfig,
                                plan.Operation() == FlashOperation::kRead
                                    ? std::string("read plan carries an image")
                                    : std::format("ROM file must be exactly 0x{:x} bytes", device.romsize));
                }
                return {};
            });
}

// Builds a kernel-loaded whole-ROM family's plan on a device the caller has
// already validated. The caller's `fields` carry the family's identity:
// operation, family, transport, target, MCU, wire parameters and
// confirmations. This adds the rest: the kernel must fit the device's kernel
// region from `kernel_load_address`, a read carries no image, any other
// operation carries exactly one whole-ROM image and erases every flash block,
// and the transfer covers the whole ROM. The built plan must then pass the
// family's own `validate`.
template <std::uint64_t KernelBlockSize>
Result<FlashPlan> BuildWholeRomKernelPlan(FlashPlanFields fields, std::optional<bytes::Bytes> image, KernelImage kernel,
                                          std::uint32_t kernel_load_address, const FlashDevice& device,
                                          Status (*validate)(const FlashPlan&))
{
    if (Status upload = ValidateKernelUpload<KernelBlockSize>(kernel.bytes.size(), kernel.load_address,
                                                              kernel_load_address, device.kblocks[0]);
        !upload.has_value())
    {
        return std::unexpected(upload.error());
    }
    const bool reading = fields.operation == FlashOperation::kRead;
    if (reading && image.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "read plans must not carry an image");
    }
    if (!reading && (!image.has_value() || image->size() != device.romsize))
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device.romsize));
    }
    fields.transfer_region = {device.fblocks[0].start, device.romsize};
    if (!reading)
    {
        fields.erase_regions = MakeEraseRegions(device);
        fields.image = std::move(image);
    }
    fields.kernel = std::move(kernel);
    return ValidateAndBuild(std::move(fields))
        .and_then([validate](FlashPlan plan) { return validate(plan).transform([&plan] { return std::move(plan); }); });
}
} // namespace fastecu::flash::detail
