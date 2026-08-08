#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "src/algorithms/protocol/bytes.h"

namespace fastecu::flash
{

enum class FlashOperation
{
    Read,
    TestWrite,
    Write,
};

enum class FlashFamily
{
    DensoSh705xEepromKline,
    DensoSh705xEepromCan,
    // Added one-by-one by the per-family tail (see spec "Explicit deferrals").
};

enum class TransportKind
{
    Kline,
    CanIso15765,
};

struct MemoryRegion
{
    std::uint32_t start;
    std::uint32_t length;
};

struct KernelImage
{
    std::string id; // diagnostic identity, not a filesystem path
    std::uint32_t load_address;
    bytes::Bytes bytes; // immutable snapshot owned by the plan
};

struct ConfirmationSpec
{
    enum class Id
    {
        BeginEepromRead,
        InspectEepromBytes,
        CycleIgnition,
    };

    Id id;
    // Stable semantic arguments; desktop owns translated title/body/buttons.
    std::vector<std::pair<std::string, std::string>> arguments;
};

} // namespace fastecu::flash

template <>
struct std::hash<fastecu::flash::ConfirmationSpec::Id>
{
    std::size_t operator()(fastecu::flash::ConfirmationSpec::Id id) const noexcept
    {
        return std::hash<int>{}(static_cast<int>(id));
    }
};

namespace fastecu::flash
{

enum class DensoSecurityVariant
{
    Stock,
    EcuTek,
    Cobb,
    EcuTekRaceRom,
};

enum class EepromReadMode : std::uint8_t
{
    Mode2 = 2,
    Mode3 = 3,
    Mode4 = 4,
};

struct DensoSh705xEepromKlinePlan
{
    EepromReadMode mode;
    DensoSecurityVariant security;
    std::uint8_t tester_id; // 0xf0 for the current family
    std::uint8_t target_id; // 0x10 for the current family; unrelated to
                            // FlashPlan::target_id(), which names the
                            // selected protocol/configuration string, not
                            // this numeric M32R/SH705x bus address.
    int initial_baud;       // 4800
    int kernel_baud;        // snapshotted resolved family value
};

struct DensoSh705xEepromCanPlan
{
    EepromReadMode mode;
    DensoSecurityVariant security;
    std::uint32_t request_id;  // 0x7e0
    std::uint32_t response_id; // 0x7e8
    int bitrate;               // 500000
    bool extended_id;          // false
};

using FamilyPlan = std::variant<
    DensoSh705xEepromKlinePlan,
    DensoSh705xEepromCanPlan>;

// Whether validate_and_build requires FlashPlanFields::kernel to be set for
// this family's plan type. Defaults true (fail-closed): a family that skips
// the kernel must opt out explicitly, right here, next to its own plan
// struct -- never by editing flash_validation.cpp.
template <typename PlanT>
inline constexpr bool family_requires_kernel_v = true;

} // namespace fastecu::flash
