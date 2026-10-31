#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"

#include "src/backend/flash/ecu/plan_primitives.h"

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

constexpr std::array kProtocols{std::string_view{"sub_ecu_denso_sh7055_02"},
                                std::string_view{"sub_ecu_denso_sh7055_02_ecutek"}};

Status ValidateIdentity(std::string_view protocol, std::string_view mcu)
{
    using enum ErrorKind;
    if (!std::ranges::contains(kProtocols, protocol))
    {
        return Fail(kInvalidConfig, std::format("Unsupported SH7055_02 protocol: {}", protocol));
    }
    if (mcu != "SH7055")
    {
        return Fail(kInvalidConfig, std::format("SH7055_02 protocol requires MCU SH7055, not {}", mcu));
    }
    return {};
}

SubaruDensoSh7055_02Plan WireParams(FlashOperation operation)
{
    // connect_bootloader() gates the SSM ECU-ID request on cmd_type being
    // "read". The selected suffix never changes these parameters.
    return {.tester_id = 0xf0, .target_id = 0x10, .read_ecu_id = operation == FlashOperation::kRead};
}

Status ValidateImage(const FlashPlan& plan, std::uint32_t romsize)
{
    if ((plan.Operation() == FlashOperation::kWrite || plan.Operation() == FlashOperation::kTestWrite) &&
        !detail::OperationImageShapeMatches(plan, romsize))
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", romsize));
    }
    return {};
}

Status ValidateKernelUpload(const KernelImage& kernel)
{
    constexpr std::uint64_t kMaxWireLength = 0x00FFFFFF;
    if (kernel.bytes.size() > kMaxWireLength)
    {
        return Fail(ErrorKind::kInvalidConfig, "SH7055_02 padded kernel plus envelope exceeds the 24-bit wire length");
    }
    const std::uint64_t padded_size = (static_cast<std::uint64_t>(kernel.bytes.size()) + 3) & ~3ULL;
    const std::uint64_t wire_length = padded_size + 4;
    if (wire_length > kMaxWireLength)
    {
        return Fail(ErrorKind::kInvalidConfig, "SH7055_02 padded kernel plus envelope exceeds the 24-bit wire length");
    }
    // The two-byte wire address selects 0xffff6000. The fixed four-byte
    // envelope occupies 0xffff6000..03, then the catalog's logical kernel
    // entry begins at 0xffff6004. The shared model region is 0x6000 bytes
    // total, so envelope plus padded payload must fit that full region.
    constexpr std::uint32_t kCanonicalKernelAddress = 0xFFFF6004;
    if (constexpr std::uint64_t kModelWireRegionLength = 0x00006000;
        kernel.load_address != kCanonicalKernelAddress || wire_length > kModelWireRegionLength)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    "SH7055_02 kernel address or padded envelope is outside the model kernel region");
    }
    return {};
}

} // namespace

Status ValidateSubaruDensoSh705502Plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (auto valid = ValidateIdentity(plan.TargetId(), plan.McuName()); !valid.has_value())
    {
        return valid;
    }
    // The wire parameters include the ECU-ID read, which only a Read performs.
    if (const auto *family = std::get_if<SubaruDensoSh7055_02Plan>(&plan.FamilyPlan());
        family == nullptr || *family != WireParams(plan.Operation()))
    {
        return Fail(kInvalidConfig, "SH7055_02 wire parameters are missing or do not match the operation");
    }
    if (!plan.EraseRegions().empty())
    {
        return Fail(kInvalidConfig, "SH7055_02 plans must not declare erase regions");
    }
    if (!plan.Kernel().has_value())
    {
        return Fail(kInvalidConfig, "SH7055_02 requires a kernel image");
    }
    if (auto valid = ValidateKernelUpload(*plan.Kernel()); !valid.has_value())
    {
        return valid;
    }
    if (!detail::RequiresOnly(plan.Confirmations(), ConfirmationSpec::Id::kCycleIgnition))
    {
        return Fail(kInvalidConfig, "SH7055_02 requires exactly the CycleIgnition confirmation");
    }
    const int index = FindFlashDeviceIndex(plan.McuName());
    if (index < 0)
    {
        return Fail(kInvalidConfig, "Unknown MCU type");
    }
    const std::uint32_t romsize = kFlashDevices[static_cast<std::size_t>(index)].romsize;
    if (!detail::TransferMatchesDevice(plan.TransferRegion(), kFlashDevices[static_cast<std::size_t>(index)]))
    {
        return Fail(kInvalidConfig, "SH7055_02 transfer region does not match the MCU");
    }
    return ValidateImage(plan, romsize);
}

Result<FlashPlan> BuildSubaruDensoSh705502Plan(FlashOperation operation, std::string_view protocol_name,
                                               std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                               KernelImage kernel)
{
    if (auto valid = ValidateIdentity(protocol_name, mcu_type); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const int index = FindFlashDeviceIndex(mcu_type);
    if (index < 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "Unknown MCU type");
    }
    const std::uint32_t romsize = kFlashDevices[static_cast<std::size_t>(index)].romsize;
    if ((operation == FlashOperation::kWrite || operation == FlashOperation::kTestWrite) &&
        (!image.has_value() || image->size() != romsize))
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", romsize));
    }
    if (auto valid = ValidateKernelUpload(kernel); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::kSubaruDensoSh705502,
        .transport = TransportKind::kKline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = MemoryRegion{kFlashDevices[static_cast<std::size_t>(index)].fblocks[0].start, romsize},
        .erase_regions = {},
        .image = operation == FlashOperation::kRead ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
        .family_plan = WireParams(operation),
        .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition}},
    };
    return ValidateAndBuild(std::move(fields))
        .and_then([](FlashPlan plan)
                  { return ValidateSubaruDensoSh705502Plan(plan).transform([&] { return std::move(plan); }); });
}
std::span<const std::string_view> SubaruDensoSh705502Protocols()
{
    return kProtocols;
}

} // namespace fastecu::flash
