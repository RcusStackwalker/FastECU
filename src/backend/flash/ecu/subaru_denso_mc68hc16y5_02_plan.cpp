#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"

#include <format>
#include <optional>
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

// mainwindow.cpp:1250-1258: sub_ecu_denso_mc68hc16y5_02(_ecutek)? and the
// reachable-but-quirky _02_tpu (see spec) all construct this class.
// Revision 04, which declared no supported operation, was deleted with the
// built-in catalog and is now an unknown name like any other.
Status ValidateIdentity(std::string_view protocol, std::string_view mcu)
{
    using enum ErrorKind;
    if (protocol != "sub_ecu_denso_mc68hc16y5_02" && protocol != "sub_ecu_denso_mc68hc16y5_02_ecutek" &&
        protocol != "sub_ecu_denso_mc68hc16y5_02_tpu")
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
    // flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:126-137 (response
    // selection), 204-242 (baud/encryption/magic selection). Only "_ecutek"
    // is reachable via the built-in catalog (see spec's "_cobb" note); every other
    // accepted name takes the stock branch.
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

} // namespace

Status ValidateSubaruDensoMc68hc16y502Plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (plan.Family() != FlashFamily::kSubaruDensoMc68hc16y502 || plan.Transport() != TransportKind::kKline)
    {
        return Fail(kInvalidConfig, "plan is not for MC68HC16Y5_02");
    }
    const auto *family = std::get_if<SubaruDensoMc68hc16y5_02Plan>(&plan.FamilyPlan());
    if (family == nullptr)
    {
        return Fail(kInvalidConfig, "MC68HC16Y5_02 wire parameters are missing");
    }
    if (auto valid = ValidateIdentity(plan.TargetId(), plan.McuName()); !valid.has_value())
    {
        return valid;
    }
    if (const SubaruDensoMc68hc16y5_02Plan expected = WireParams(plan.TargetId());
        family->connect_baud != expected.connect_baud || family->kernel_baud != expected.kernel_baud ||
        family->encryption_xor != expected.encryption_xor || family->kernel_magic != expected.kernel_magic ||
        family->bootloader_ok != expected.bootloader_ok)
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
    const int index = FindFlashDeviceIndex(plan.McuName());
    if (index < 0)
    {
        return Fail(kInvalidConfig, "Unknown MCU type");
    }
    const std::uint32_t romsize = kFlashDevices[index].romsize;
    if (plan.TransferRegion().start != kFlashDevices[index].fblocks[0].start || plan.TransferRegion().length != romsize)
    {
        return Fail(kInvalidConfig, "MC68HC16Y5_02 transfer region does not match the MCU");
    }
    if (auto valid = ValidateOperation(plan.TargetId(), plan.Operation()); !valid.has_value())
    {
        return valid;
    }
    if (plan.Operation() == FlashOperation::kWrite || plan.Operation() == FlashOperation::kTestWrite)
    {
        const std::optional<memory::MemoryImage> image = plan.RomImage();
        if (!image.has_value())
        {
            return Fail(kInvalidConfig, "MC68HC16Y5_02 write requires a ROM image");
        }
        return CheckFlashBlocksPlaced(*image, kFlashDevices[index]);
    }
    return {};
}

Result<FlashPlan> BuildSubaruDensoMc68hc16y502Plan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<memory::MemoryImage> image,
                                                   KernelImage kernel)
{
    if (auto valid = ValidateIdentity(protocol_name, mcu_type); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (auto valid = ValidateOperation(protocol_name, operation); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (auto valid = ValidateKernelUpload(kernel); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const int index = FindFlashDeviceIndex(mcu_type);
    if (index < 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "Unknown MCU type");
    }
    const std::uint32_t romsize = kFlashDevices[index].romsize;
    std::optional<bytes::Bytes> file;
    std::optional<memory::MemoryMap> file_map;
    if (operation == FlashOperation::kWrite || operation == FlashOperation::kTestWrite)
    {
        if (!image.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5_02 write requires a ROM image");
        }
        if (auto placed = CheckFlashBlocksPlaced(*image, kFlashDevices[index]); !placed.has_value())
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
        .transfer_region = MemoryRegion{kFlashDevices[index].fblocks[0].start, romsize},
        .erase_regions = {}, // per-block erase happens inside the write executor
                             // (blank-page-per-modified-block, legacy
                             // flash_block():950-992), not a fixed up-front set
        .image = std::move(file),
        .image_map = std::move(file_map),
        .kernel = std::move(kernel),
        .family_plan = WireParams(protocol_name),
    };
    auto plan = ValidateAndBuild(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = ValidateSubaruDensoMc68hc16y502Plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
