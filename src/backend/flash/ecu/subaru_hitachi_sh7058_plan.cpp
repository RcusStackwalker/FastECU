#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/flash/ecu/plan_primitives.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_types.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
constexpr std::string_view kProtocol = "sub_ecu_hitachi_sh7058_can";
constexpr std::string_view kMcu = "SH7058_1block";
constexpr std::uint32_t kSize = 0x100000;

// Read uploads the 1 MiB window above the ROM over K-Line after the operator
// starts it; Write erases and programs the whole ROM over CAN.
MemoryRegion TransferFor(bool read)
{
    return {read ? kSize : 0, kSize};
}

std::vector<MemoryRegion> EraseFor(bool read)
{
    return read ? std::vector<MemoryRegion>{} : std::vector<MemoryRegion>{{0, kSize}};
}

std::vector<ConfirmationSpec> ConfirmationsFor(bool read)
{
    return read ? std::vector{ConfirmationSpec{.id = ConfirmationSpec::Id::kStartKlineRead}}
                : std::vector<ConfirmationSpec>{};
}

template <typename Wire> bool HasDefaultWire(const FlashPlan& plan, TransportKind transport)
{
    const auto *wire = plan.FamilyPlan().As<Wire>();
    return plan.Transport() == transport && wire != nullptr && *wire == Wire{};
}

bool DeviceMatches(const FlashDevice *device)
{
    return device != nullptr && device->romsize == kSize && device->numblocks == 1 && device->fblocks != nullptr &&
           device->fblocks[0].start == 0 && device->fblocks[0].len == kSize;
}
} // namespace

Status ValidateSubaruHitachiSh7058Plan(const FlashPlan& plan)
{
    if (plan.Family() != FlashFamily::kSubaruHitachiSh7058 || plan.TargetId() != kProtocol || plan.McuName() != kMcu)
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid SH7058 identity");
    }
    if (plan.Operation() == FlashOperation::kTestWrite)
    {
        return Fail(ErrorKind::kUnsupported, "SH7058 test write performs a live erase and is unsupported");
    }
    const bool read = plan.Operation() == FlashOperation::kRead;
    if (read ? !HasDefaultWire<SubaruHitachiSh7058KlinePlan>(plan, TransportKind::kKline)
             : !HasDefaultWire<SubaruHitachiSh7058CanPlan>(plan, TransportKind::kCanIso15765))
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid SH7058 transport or wire parameters");
    }
    if (plan.TransferRegion() != TransferFor(read) || !std::ranges::equal(plan.EraseRegions(), EraseFor(read)) ||
        !detail::OperationImageShapeMatches(plan, kSize) || plan.Kernel().has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid SH7058 flash geometry");
    }
    if (!std::ranges::equal(plan.Confirmations(), ConfirmationsFor(read)))
    {
        return Fail(ErrorKind::kInvalidConfig,
                    "SH7058 Read requires exactly the StartKlineRead confirmation; Write requires none");
    }
    return {};
}

Result<FlashPlan> BuildSubaruHitachiSh7058Plan(FlashOperation operation, std::string_view protocol,
                                               std::string_view mcu, std::optional<bytes::Bytes> image)
{
    if (protocol != kProtocol || mcu != kMcu)
    {
        return Fail(ErrorKind::kInvalidConfig, "unsupported SH7058 protocol or MCU");
    }
    if (!DeviceMatches(FindFlashDevice(kMcu)))
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid SH7058 device geometry");
    }
    if (operation == FlashOperation::kTestWrite)
    {
        return Fail(ErrorKind::kUnsupported, "SH7058 test write performs a live erase and is unsupported");
    }
    const bool read = operation == FlashOperation::kRead;
    if (read ? image.has_value() : (!image.has_value() || image->size() != kSize))
    {
        return Fail(ErrorKind::kInvalidConfig, "SH7058 ROM image must be exactly 1 MiB for write");
    }
    auto plan = ValidateAndBuild({
        .operation = operation,
        .family = FlashFamily::kSubaruHitachiSh7058,
        .transport = read ? TransportKind::kKline : TransportKind::kCanIso15765,
        .target_id = std::string(protocol),
        .mcu_name = std::string(mcu),
        .transfer_region = TransferFor(read),
        .erase_regions = EraseFor(read),
        .image = std::move(image),
        .kernel = std::nullopt,
        .family_plan = read ? FamilyPlan{SubaruHitachiSh7058KlinePlan{}} : FamilyPlan{SubaruHitachiSh7058CanPlan{}},
        .confirmations = ConfirmationsFor(read),
    });
    if (!plan.has_value())
    {
        return plan;
    }
    if (auto valid = ValidateSubaruHitachiSh7058Plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
constexpr std::array kExportedProtocols{kProtocol};

std::span<const std::string_view> SubaruHitachiSh7058Protocols()
{
    return kExportedProtocols;
}

Status PlanValidator<SubaruHitachiSh7058CanPlan>::Validate(const FlashPlan& plan)
{
    if (auto valid = ValidateSubaruHitachiSh7058Plan(plan); !valid.has_value())
    {
        return valid;
    }
    return plan.Operation() == FlashOperation::kWrite ? Status{}
                                                      : Fail(ErrorKind::kUnsupported, "SH7058 CAN supports write only");
}

Status PlanValidator<SubaruHitachiSh7058KlinePlan>::Validate(const FlashPlan& plan)
{
    if (auto valid = ValidateSubaruHitachiSh7058Plan(plan); !valid.has_value())
    {
        return valid;
    }
    return plan.Operation() == FlashOperation::kRead
               ? Status{}
               : Fail(ErrorKind::kUnsupported, "SH7058 K-Line supports read only");
}

} // namespace fastecu::flash
