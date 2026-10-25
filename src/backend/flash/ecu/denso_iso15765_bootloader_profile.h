#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "src/backend/protocol/uds/exchange_policy.h"

namespace fastecu::flash
{

// Which fire-and-forget run an in-car arm sends.
enum class DensoInCarRun
{
    kN83m,
    kSh,
};

// How the bench arm's session exchange treats a reply other than `50 43`:
// kFatal aborts on a wrong or absent reply; kTolerantProbe logs a wrong reply
// and aborts on an absent one.
enum class DensoBenchSessionReply
{
    kFatal,
    kTolerantProbe,
};

// How read_memory's two dump-setup exchanges treat a reply other than the
// expected one: kFatal aborts on a wrong or absent reply; kTolerantReply logs
// both and aborts only on a transport failure.
enum class DensoReadSetupReply
{
    kFatal,
    kTolerantReply,
};

// The connect's ECU identity query: the 0xAA ECU-ID service, or a 0x22 read
// of DID F182 that logs five bytes.
enum class DensoIdentityQuery
{
    kEcuIdService,
    kDidF182,
};

// The wording of the read-setup and write-block log lines: legacy's
// "Settting dump start & length" / "Flash block addr: ..." pair, or
// "Setting flash start: ... and length: ..." for both.
enum class DensoRegionLog
{
    kDumpAndBlock,
    kStartAndLength,
};

// The bench programming arm: settle, the bench session (10 43), seed/key, then
// the bench kernel jump (10 42, at most 50 tries).
struct DensoBenchArm
{
    std::chrono::milliseconds settle;
    DensoBenchSessionReply session_reply;
    std::chrono::milliseconds session_timeout;
    std::chrono::milliseconds jump_first_read;
    std::chrono::milliseconds jump_loop_read;
    bool discard_first_reply;
};

// A Denso ISO-15765 bootloader family's dialect profile: every way the family
// deviates from its dialect siblings. The order of operations belongs to
// DensoIso15765BootloaderExecutor.
struct DensoIso15765BootloaderProfile
{
    std::string_view unit; // RunBootloaderOperation's unit, e.g. "Subaru Denso SH72531"
    std::string_view obk_active_log;
    DensoIdentityQuery identity_query;
    std::chrono::milliseconds probe_timeout;
    DensoInCarRun in_car_run;
    std::chrono::milliseconds in_car_timeout;
    DensoBenchArm bench;
    DensoReadSetupReply read_setup_reply;
    uds::ExchangePolicy read_setup_policy;
    bool pace_pages;
    DensoRegionLog region_log;
    int write_block_index;
    uds::ExchangePolicy checksum_policy;
    std::uint32_t image_start; // byte 0 of the plan image
};

// Specialised next to each family's plan type as
// `static constexpr DensoIso15765BootloaderProfile kValue`.
template <typename Plan> struct DensoIso15765BootloaderProfileOf;

} // namespace fastecu::flash
