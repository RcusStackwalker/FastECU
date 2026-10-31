#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "src/algorithms/memory/memory_image.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/config/catalog.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/flash/flash_family.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
// MC68HC16Y5_02. tester_id/target_id are NOT carried here: legacy
// flash_ecu_subaru_denso_mc68hc16y5_02_operation.h declares them but the
// .cpp never reads them after execute() assigns 0xf0/0x10 (verified by
// grep across every method) -- dead members, not ported.
struct SubaruDensoMc68hc16y5_02Plan
{
    int connect_baud;                          // 9600 (connect_bootloader)
    int kernel_baud;                           // 11700 (_ecutek) or 9600 (stock/_cobb)
    std::uint8_t encryption_xor;               // 0x51 (_ecutek) or 0x55 (stock/_cobb)
    std::uint16_t kernel_magic;                // 0x3940 (_ecutek) or 0x3941 (stock/_cobb)
    std::array<std::uint8_t, 3> bootloader_ok; // WRX02 init OK response: stock/_cobb
                                               // {0x4D,0x00,0xB3}, _ecutek {0x4C,0x00,0xB4}

    bool operator==(const SubaruDensoMc68hc16y5_02Plan&) const = default;
};

template <> struct FamilyTraits<SubaruDensoMc68hc16y5_02Plan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoMc68hc16y502;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = true;
};

template <> struct PlanValidator<SubaruDensoMc68hc16y5_02Plan>
{
    static Status Validate(const FlashPlan& plan);
};

// A Write/TestWrite `image` is the ROM file placed by its memory map; every
// flash block of the MCU must be writable ROM file bytes in it, whatever the
// file's size (ADR 0020).
Result<FlashPlan> BuildSubaruDensoMc68hc16y502Plan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<memory::MemoryImage> image,
                                                   KernelImage kernel);
Status ValidateSubaruDensoMc68hc16y502Plan(const FlashPlan& plan);

// The flash workflow's plan preparation for this family. A Write/TestWrite
// `rom_file` is placed by the protocol's memory map for its size (ADR 0020),
// then the builder checks the protocol, MCU and placement against a
// placeholder kernel, so a protocol or MCU the family rejects fails before
// the kernel file is read. Only then is the protocol's kernel file read and
// the plan built with it.
Result<FlashPlan> PrepareSubaruDensoMc68hc16y502Plan(FlashOperation operation, const config::ProtocolSpec& protocol,
                                                     const config::ConfigPaths& paths,
                                                     std::optional<bytes::Bytes> rom_file, IFileRepository& files);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDensoMc68hc16y502Protocols();
} // namespace fastecu::flash
