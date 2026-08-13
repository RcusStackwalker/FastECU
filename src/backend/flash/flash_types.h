#pragma once
#include <array>
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
    // Step 5 tail, wave 0. Serves all four mitsu_ecu_m32r_can capacity and
    // vendor-authorization variants; both properties are plan fields, not
    // separate families, matching the legacy class this replaces.
    MitsuColtM32rCan,
    SubaruMitsuM32rKline,
    SubaruHitachiM32rKline,
    // Step 5 tail, wave 2.
    SubaruDensoMc68hc16y5_02,
    SubaruDensoSh7055_02,
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
    bool use_vendor_challenge; // selected by the protocol identifier
    bytes::Byte session_id;    // kSessionBootload (0x85) for Read and Write
};

struct SubaruMitsuM32rKlinePlan
{
    std::uint8_t tester_id;
    std::uint8_t target_id;
    int initial_baud;
    int flash_baud;
    std::uint32_t chunk_size;
    bytes::Byte unread_prefix_fill;
};

enum class HitachiM32rKlineSessionMode
{
    Normal,
    Recovery,
};

struct SubaruHitachiM32rKlinePlan
{
    HitachiM32rKlineSessionMode session_mode;
    std::uint8_t tester_id;
    std::uint8_t target_id;
    int initial_baud;
    int write_baud;
    int read_baud;
    std::uint32_t chunk_size;
    std::uint32_t read_address_bias;
};

// MC68HC16Y5_02, wave 2. tester_id/target_id are NOT carried here: legacy
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
};

// SH7055_02, wave 2. Unlike MC68, tester_id/target_id ARE live -- the one
// surviving SSM-framed exchange (SID 0xBF ECU-ID read, read-only, Read
// operation only) uses them via SsmProtocol::addHeader().
struct SubaruDensoSh7055_02Plan
{
    std::uint8_t tester_id; // 0xf0
    std::uint8_t target_id; // 0x10
    bool read_ecu_id;       // true iff FlashOperation::Read
};

using FamilyPlan = std::variant<
    DensoSh705xEepromKlinePlan,
    DensoSh705xEepromCanPlan,
    MitsuColtM32rCanPlan,
    SubaruMitsuM32rKlinePlan,
    SubaruHitachiM32rKlinePlan,
    SubaruDensoMc68hc16y5_02Plan,
    SubaruDensoSh7055_02Plan>;

// Whether validate_and_build requires FlashPlanFields::kernel to be set for
// this family's plan type. Defaults true (fail-closed): a family that skips
// the kernel must opt out explicitly, right here, next to its own plan
// struct -- never by editing flash_validation.cpp.
template <typename PlanT>
inline constexpr bool family_requires_kernel_v = true;

// Mitsu Colt CAN drives the ECU's own vendor bootloader and uploads only
// compile-time RAM helper routines, not a loaded kernel image.
template <>
inline constexpr bool family_requires_kernel_v<MitsuColtM32rCanPlan> = false;

template <>
inline constexpr bool family_requires_kernel_v<SubaruMitsuM32rKlinePlan> = false;

template <>
inline constexpr bool family_requires_kernel_v<SubaruHitachiM32rKlinePlan> = false;

} // namespace fastecu::flash
