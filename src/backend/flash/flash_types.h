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
    // Step 5 tail, wave 0. Serves both mitsu_ecu_m32r_can and
    // mitsu_ecu_m32r_can_vendor_ext; the vendor challenge is a plan flag,
    // not a separate family, matching the legacy class it replaces.
    MitsuColtM32rCan,
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
        // Step 5 tail, wave 0. Both are collected by the desktop dialog
        // BEFORE the executor starts: a synchronous, dialog-free executor
        // cannot block mid-run for a human answer. Presence in
        // FlashPlan::confirmations() therefore means "granted" -- an
        // operator who declines either one causes the dialog to never build
        // a plan at all.
        EraseTrigger,
        TopRegionBootstrap,
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

struct MitsuColtM32rCanPlan
{
    std::uint32_t request_id;  // 0x7e0
    std::uint32_t response_id; // 0x7e8
    int bitrate;               // 500000
    bool extended_id;          // false -- build_request() hardcodes the
                               // 11-bit physical request id
    bool use_vendor_challenge; // mitsu_ecu_m32r_can_vendor_ext only
    bytes::Byte session_id;    // kSessionBasic (0x81) for Read,
                               // kSessionBootload (0x85) for Write
};

using FamilyPlan = std::variant<
    DensoSh705xEepromKlinePlan,
    DensoSh705xEepromCanPlan,
    MitsuColtM32rCanPlan>;

} // namespace fastecu::flash
