#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/config/catalog.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/flash/flash_family.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
// Bootmode Write is two attempts: the kernel upload into
// the M32R boot ROM, then erase and program through that kernel. Each attempt
// is its own family so each executor's check_family() rejects the other's
// plan. The ROM and kernel sizes come from the plan's transfer region.
struct SubaruUnisiaJecsM32rBootModeKernelPlan
{
    int initial_baud;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};

// Requires no kernel: The kernel attempt carries the cfg kernel as its plan
// image, as 6c-1 BDM does: the _bootmode cfg entries declare no kernel_addr,
// and the M32R boot ROM places the kernel itself. The program attempt uploads
// nothing.
template <> struct FamilyTraits<SubaruUnisiaJecsM32rBootModeKernelPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruUnisiaJecsM32rBootModeKernelPlan>
{
    static Status Validate(const FlashPlan& plan);
};

struct SubaruUnisiaJecsM32rBootModeProgramPlan
{
    int initial_baud;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};

template <> struct FamilyTraits<SubaruUnisiaJecsM32rBootModeProgramPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruUnisiaJecsM32rBootModeProgramPlan>
{
    static Status Validate(const FlashPlan& plan);
};

// Exact protocol/MCU pairs sub_ecu_unisia_jecs_20_bootmode /
// M32R_128KB and sub_ecu_unisia_jecs_30_bootmode / M32R_256KB, Write only
// (Read goes through the 6c-3 K-Line family). Write is two plans run as two
// attempts: the kernel upload, then erase and program. Both carry
// ApplyBootModeVoltages, collected by the desktop workflow before either runs.
Result<FlashPlan> BuildSubaruUnisiaJecsM32rBootmodeKernelPlan(FlashOperation operation, std::string_view protocol_name,
                                                              std::string_view mcu_type, bytes::Bytes kernel);
Result<FlashPlan> BuildSubaruUnisiaJecsM32rBootmodeProgramPlan(FlashOperation operation, std::string_view protocol_name,
                                                               std::string_view mcu_type,
                                                               std::optional<bytes::Bytes> image);
// Validates a plan of either bootmode family.
Status ValidateSubaruUnisiaJecsM32rBootmodePlan(const FlashPlan& plan);

// The plans one boot-mode workflow runs: Read is the 6c-3 K-Line read plan;
// Write is the kernel upload followed by erase and program.
struct SubaruUnisiaJecsM32rBootModePlans
{
    std::optional<FlashPlan> first;   // the Read plan, or the kernel upload
    std::optional<FlashPlan> program; // Write only
};

// The flash workflow's plan preparation for this family, run before Begin so a
// missing kernel or a wrong-size image fails before any prompt. A Write builds
// the program plan first: it needs no I/O and rejects TestWrite and a wrong
// `rom_file` before the protocol's kernel file is read.
Result<SubaruUnisiaJecsM32rBootModePlans> PrepareSubaruUnisiaJecsM32rBootModePlans(FlashOperation operation,
                                                                                   const config::ProtocolSpec& protocol,
                                                                                   const config::ConfigPaths& paths,
                                                                                   std::optional<bytes::Bytes> rom_file,
                                                                                   IFileRepository& files);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruUnisiaJecsM32rBootModeProtocols();
} // namespace fastecu::flash
