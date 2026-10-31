#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_types.h"

#include "src/backend/flash/ecu/plan_primitives.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

// Every flash block of `device` is writable ROM file bytes in `image`.
Status CheckFlashBlocksPlaced(const memory::MemoryImage& image, const FlashDevice& device)
{
    for (unsigned block_no = 0; block_no < device.numblocks; ++block_no)
    {
        const auto& block = device.fblocks[block_no];
        if (const auto placed = image.CheckWrite(memory::FlashAddress{block.start}, memory::ByteCount{block.len});
            !placed.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("ROM file's memory map does not hold flash block 0x{:x} as writable ROM file "
                                    "bytes: {}",
                                    block.start, placed.error().detail));
        }
    }
    return {};
}

constexpr std::array kProtocols{std::string_view{"sub_ecu_denso_mc68hc16y5_02"},
                                std::string_view{"sub_ecu_denso_mc68hc16y5_02_ecutek"},
                                std::string_view{"sub_ecu_denso_mc68hc16y5_02_tpu"}};

// mainwindow.cpp:1250-1258: sub_ecu_denso_mc68hc16y5_02(_ecutek)? and the
// reachable-but-quirky _02_tpu (see spec) all construct this class.
// Revision 04, which declared no supported operation, was deleted with the
// built-in catalog and is now an unknown name like any other.
Status ValidateIdentity(std::string_view protocol, std::string_view mcu)
{
    using enum ErrorKind;
    if (!std::ranges::contains(kProtocols, protocol))
    {
        return Fail(kInvalidConfig, std::format("Unsupported MC68HC16Y5_02 protocol: {}", protocol));
    }
    if (const std::string_view expected_mcu =
            protocol == "sub_ecu_denso_mc68hc16y5_02_tpu" ? "MC68HC16Y5_TPU" : "MC68HC16Y5";
        mcu != expected_mcu)
    {
        return Fail(kInvalidConfig, std::format("protocol {} requires MCU {}, not {}", protocol, expected_mcu, mcu));
    }
    return {};
}

Status ValidateOperation(std::string_view protocol, FlashOperation operation)
{
    if (protocol == "sub_ecu_denso_mc68hc16y5_02_tpu" && operation != FlashOperation::kRead)
    {
        return Fail(
            ErrorKind::kUnsupported,
            "the built-in catalog declares no supported write or test_write operation for the MC68HC16Y5 TPU variant");
    }
    return {};
}

SubaruDensoMc68hc16y5_02Plan WireParams(std::string_view protocol)
{
    // Legacy response, baud, encryption and magic selection. Only
    // "_ecutek" is reachable via the built-in catalog (see spec's "_cobb" note);
    // every other accepted name takes the stock branch.
    if (protocol.ends_with("_ecutek"))
    {
        return {.connect_baud = 9600,
                .kernel_baud = 11700,
                .encryption_xor = 0x51,
                .kernel_magic = 0x3940,
                .bootloader_ok = {0x4C, 0x00, 0xB4}};
    }
    return {.connect_baud = 9600,
            .kernel_baud = 9600,
            .encryption_xor = 0x55,
            .kernel_magic = 0x3941,
            .bootloader_ok = {0x4D, 0x00, 0xB3}};
}

Status ValidateKernelUpload(const KernelImage& kernel)
{
    // Legacy upload_kernel() serializes only address bits 23..8, so the low
    // byte is implicit zero. The catalog and shared device table both place
    // this kernel at the one canonical 0x20000 model region.
    constexpr std::uint32_t kKernelStart = 0x00020000;
    constexpr std::uint64_t kKernelLength = 0x00008000;
    constexpr std::uint64_t kMaxWireLength = 0x00ffffff;
    if (kernel.load_address != kKernelStart)
    {
        return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5_02 kernel address is not the canonical wire address");
    }
    const std::uint64_t padded_size = (static_cast<std::uint64_t>(kernel.bytes.size()) + 0x0f) & ~0x0fULL;
    if (padded_size > kMaxWireLength)
    {
        return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5_02 padded kernel exceeds the 24-bit wire length");
    }
    if (padded_size > kKernelLength)
    {
        return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5_02 padded kernel is outside the model kernel region");
    }
    return {};
}

// The transfer covers the MCU's ROM, and a write's image places every flash block.
Status ValidateRomPlacement(const FlashPlan& plan)
{
    using enum ErrorKind;
    const int index = FindFlashDeviceIndex(plan.McuName());
    if (index < 0)
    {
        return Fail(kInvalidConfig, "Unknown MCU type");
    }
    if (!detail::TransferMatchesDevice(plan.TransferRegion(), kFlashDevices[static_cast<std::size_t>(index)]))
    {
        return Fail(kInvalidConfig, "MC68HC16Y5_02 transfer region does not match the MCU");
    }
    if (auto valid = ValidateOperation(plan.TargetId(), plan.Operation()); !valid.has_value())
    {
        return valid;
    }
    if (plan.Operation() == FlashOperation::kRead)
    {
        return {};
    }
    const std::optional<memory::MemoryImage>& image = plan.RomImage();
    if (!image.has_value())
    {
        return Fail(kInvalidConfig, "MC68HC16Y5_02 write requires a ROM image");
    }
    return CheckFlashBlocksPlaced(*image, kFlashDevices[static_cast<std::size_t>(index)]);
}

} // namespace

Status ValidateSubaruDensoMc68hc16y502Plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    const auto *family = plan.FamilyPlan().As<SubaruDensoMc68hc16y5_02Plan>();
    if (family == nullptr)
    {
        return Fail(kInvalidConfig, "MC68HC16Y5_02 wire parameters are missing");
    }
    if (auto valid = ValidateIdentity(plan.TargetId(), plan.McuName()); !valid.has_value())
    {
        return valid;
    }
    if (*family != WireParams(plan.TargetId()))
    {
        return Fail(kInvalidConfig, "MC68HC16Y5_02 wire parameters are invalid");
    }
    if (!plan.Kernel().has_value())
    {
        return Fail(kInvalidConfig, "MC68HC16Y5_02 requires a kernel image");
    }
    if (auto valid = ValidateKernelUpload(*plan.Kernel()); !valid.has_value())
    {
        return valid;
    }
    if (!plan.EraseRegions().empty())
    {
        return Fail(kInvalidConfig, "MC68HC16Y5_02 plans must not declare erase regions");
    }
    if (!plan.Confirmations().empty())
    {
        return Fail(kInvalidConfig, "MC68HC16Y5_02 plans must not declare confirmations");
    }
    return ValidateRomPlacement(plan);
}

Result<FlashPlan> BuildSubaruDensoMc68hc16y502Plan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<memory::MemoryImage> image,
                                                   KernelImage kernel)
{
    if (const Status valid = ValidateIdentity(protocol_name, mcu_type)
                                 .and_then([&] { return ValidateOperation(protocol_name, operation); })
                                 .and_then([&] { return ValidateKernelUpload(kernel); });
        !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const int index = FindFlashDeviceIndex(mcu_type);
    if (index < 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "Unknown MCU type");
    }
    const std::uint32_t romsize = kFlashDevices[static_cast<std::size_t>(index)].romsize;
    std::optional<bytes::Bytes> file;
    std::optional<memory::MemoryMap> file_map;
    if (operation == FlashOperation::kWrite || operation == FlashOperation::kTestWrite)
    {
        if (!image.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5_02 write requires a ROM image");
        }
        if (auto placed = CheckFlashBlocksPlaced(*image, kFlashDevices[static_cast<std::size_t>(index)]);
            !placed.has_value())
        {
            return std::unexpected(placed.error());
        }
        file = bytes::Bytes(image->File().begin(), image->File().end());
        file_map = image->Map();
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::kSubaruDensoMc68hc16y502,
        .transport = TransportKind::kKline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        // A byte count from flash start: romsize is the packed 160 KiB file's
        // size, not the ECU range the write spans (0x0-0x2FFFF with RAM at
        // 0x20000-0x27FFF). The executor writes the flash blocks through the
        // plan's RomImage(), which ValidateAndBuild places by image_map.
        .transfer_region = MemoryRegion{kFlashDevices[static_cast<std::size_t>(index)].fblocks[0].start, romsize},
        .erase_regions = {}, // per-block erase happens inside the write executor
                             // (blank-page-per-modified-block, legacy
                             // flash_block()), not a fixed up-front set
        .image = std::move(file),
        .image_map = std::move(file_map),
        .kernel = std::move(kernel),
        .family_plan = WireParams(protocol_name),
    };
    return ValidateAndBuild(std::move(fields))
        .and_then([](FlashPlan plan)
                  { return ValidateSubaruDensoMc68hc16y502Plan(plan).transform([&] { return std::move(plan); }); });
}

std::span<const std::string_view> SubaruDensoMc68hc16y502Protocols()
{
    return kProtocols;
}

Status PlanValidator<SubaruDensoMc68hc16y5_02Plan>::Validate(const FlashPlan& plan)
{
    return ValidateSubaruDensoMc68hc16y502Plan(plan);
}

} // namespace fastecu::flash
