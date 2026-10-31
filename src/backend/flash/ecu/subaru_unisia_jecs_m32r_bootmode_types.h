#pragma once

#include <cstdint>

#include "src/backend/flash/flash_family.h"
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

// The kernel attempt carries the cfg kernel as its plan
// image, as 6c-1 BDM does: the _bootmode cfg entries declare no kernel_addr,
// and the M32R boot ROM places the kernel itself. The program attempt uploads
// nothing.
template <> struct FamilyTraits<SubaruUnisiaJecsM32rBootModeKernelPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel;
    static constexpr TransportKind kTransport = TransportKind::kKline;
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
};

template <> struct PlanValidator<SubaruUnisiaJecsM32rBootModeProgramPlan>
{
    static Status Validate(const FlashPlan& plan);
};
} // namespace fastecu::flash
