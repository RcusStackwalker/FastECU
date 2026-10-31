#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/protocol/uds/uds_client.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

constexpr std::uint32_t kDiagnosticRequestId = 0x7E0;
// Legacy serial_read_timeout: every connect_bootloader() and read_mem() read,
// and the per-block 0x34 window setup in reflash_block.
constexpr auto kResponseTimeout = 2000ms;
// Legacy receive_timeout: the 0xB6 data-frame read only.
constexpr auto kDataFrameTimeout = 500ms;
// Legacy serial_read_long_timeout: the retried 0x37 close and 0x31 02 02 01
// checksum reads.
constexpr auto kRetryTimeout = 800ms;
// Legacy erase_mem reads with a literal 200 ms.
constexpr auto kEraseTimeout = 200ms;
constexpr auto kShortDelay = 50ms;
constexpr auto kJumpDelay = 200ms;
// Legacy read_mem's delay(1) after every dumped page.
constexpr auto kPageDelay = 1ms;
// Legacy reflash_block's delay(200) before every 0xB6 read and its delay(100)
// between a closed block and its checksum.
constexpr auto kDataFrameDelay = 200ms;
constexpr auto kChecksumDelay = 100ms;
// Legacy erase_mem's delay(500) before the erase read.
constexpr auto kEraseDelay = 500ms;
// Legacy reflash_block retries both the close and the checksum 20 times;
// exhausting either fails the block.
constexpr int kRetryAttempts = 20;

// Directly compared against legacy generate_seed_key(): the 32-entry index
// transformation is byte-identical to SsmProtocol::kIndexTransformationStock.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{
    0xF2CA, 0x2417, 0x21DE, 0x8475, 0x39AB, 0xF767, 0x6204, 0x6BE0,
    0xBC63, 0x5988, 0x2845, 0x9846, 0xEB97, 0x99DE, 0xC7DB, 0xEFAE,
};

// Legacy decrypt_payload: encrypt_payload's four words in reverse order. The
// read path decrypts what the kernel dumps; the write path encrypts the image
// before the first frame goes out.
constexpr std::array<std::uint16_t, 4> kDecryptTable{0x1075, 0x9E51, 0x8BEF, 0x3B61};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x3B61, 0x8BEF, 0x9E51, 0x1075};

// Legacy write_mem's block_modified table: 16 entries for an 11-block
// M32R_512KB, so entries 11-15 are never consulted. Blocks 0-2 (0x0000, 0x4000,
// 0x6000) hold the bootloader and are deliberately left alone; only blocks 3-10
// (0x8000 through 0x80000) are reflashed.
constexpr std::array<bool, 16> kBlockModified{
    false, false, false, true, true, true, true, true, true, true, true, false, false, false, false, false,
};

bytes::Bytes SeedKey(bytes::ByteView seed)
{
    return ssm_protocol::CalculateSeedKey(seed, kSeedKeyTable, ssm_protocol::kStockPairTable);
}

// Legacy composes ecuCalDef->RomId as "<CALID>_<TCUID>_": the TCU ID step sets
// RomId = "<TCUID>_" and the CAL ID step then INSERTS "<CALID>_" at the front,
// giving "<CALID>_<TCUID>_". Both legacy steps are non-fatal on a short or
// mismatched response and could leave a partial "<TCUID>_"-only or
// "<CALID>_"-only id; the port only reports a rom_id when BOTH decoded cleanly
// (matched prefix and long enough), so a caller either gets the well-formed
// identifier legacy's success path produces or none at all -- never a
// half-built one that could still end up naming a saved dump.
struct Ctx
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    IClock& clock;
    uds::UdsClient& diagnostic;
    uds::UdsClient& kernel;
    uds::IUdsChannel& kernel_channel;
};

constexpr std::string_view kRejectionPrefix = "Wrong response from TCU: ";

// Only the two per-step timing knobs this executor ever sets; every other
// ExchangePolicy field keeps its default.
uds::ExchangePolicy Policy(std::chrono::milliseconds pre_read_delay, std::chrono::milliseconds read_timeout)
{
    uds::ExchangePolicy policy;
    policy.pre_read_delay = pre_read_delay;
    policy.read_timeout = read_timeout;
    return policy;
}

Result<bytes::Bytes> Fatal(const Ctx& ctx, uds::UdsClient& client, bytes::ByteView pdu,
                           const uds::ExchangePolicy& policy, bytes::ByteView expected_payload_prefix,
                           std::string_view subject, std::optional<std::size_t> min_payload_size = std::nullopt)
{
    return FatalQuery(UdsExchangeContext{client, policy, ctx.cancellation, ctx.events}, pdu, expected_payload_prefix,
                      kRejectionPrefix, subject, min_payload_size);
}

// Legacy logs a missing or wrong answer and carries on (its `return
// STATUS_ERROR` lines are commented out). Only a silent TCU or one that
// answered wrongly is stepped over; cancellation and transport errors are not
// ECU content mismatches and propagate. The shared NonFatalQuery does not fit:
// it returns void and swallows every error.
Result<std::optional<bytes::Bytes>> Tolerant(const Ctx& ctx, uds::UdsClient& client, bytes::ByteView pdu,
                                             const uds::ExchangePolicy& policy, bytes::ByteView expected_pdu_prefix,
                                             std::string_view label)
{
    Result<bytes::Bytes> reply = client.Request(pdu, policy, ctx.cancellation);
    if (!reply.has_value())
    {
        if (reply.error().kind == ErrorKind::kTimeout)
        {
            ctx.events.Log(LogLevel::kError, std::format("No valid response from TCU for {}", label));
            return std::optional<bytes::Bytes>{};
        }
        if (reply.error().kind == ErrorKind::kBadResponse)
        {
            ctx.events.Log(LogLevel::kError,
                           std::format("Wrong response from TCU for {}: {}", label, reply.error().detail));
            return std::optional<bytes::Bytes>{};
        }
        return std::unexpected(reply.error());
    }
    if (reply->size() < expected_pdu_prefix.size() ||
        !std::equal(expected_pdu_prefix.begin(), expected_pdu_prefix.end(), reply->begin()))
    {
        ctx.events.Log(LogLevel::kError,
                       std::format("Wrong response from TCU for {}: {}", label, bytes::ToHex(*reply)));
        return std::optional<bytes::Bytes>{};
    }
    return std::optional<bytes::Bytes>{std::move(*reply)};
}

// Step 1: a matching response means the resident kernel is already live and
// the remaining seven exchanges must not be sent. A missing or non-matching
// frame means normal initialization should continue.
Result<bool> KernelAlive(const Ctx& ctx)
{
    const bytes::Bytes probe{uds::kSidRoutineControl, 0x02, 0x02, 0x01};
    Result<bytes::Bytes> alive = ctx.kernel.Request(probe, Policy(0ms, kResponseTimeout), ctx.cancellation);
    if (!alive.has_value())
    {
        if (alive.error().kind == ErrorKind::kTimeout)
        {
            ctx.events.Log(LogLevel::kError, "No valid response from TCU");
            return false;
        }
        if (alive.error().kind == ErrorKind::kBadResponse)
        {
            ctx.events.Log(LogLevel::kError, std::format("Wrong response from TCU: {}", alive.error().detail));
            return false;
        }
        return std::unexpected(alive.error());
    }
    constexpr std::array<bytes::Byte, 4> kAliveReply{0x71, 0x02, 0x02, 0x03};
    if (alive->size() >= kAliveReply.size() && std::equal(kAliveReply.begin(), kAliveReply.end(), alive->begin()))
    {
        ctx.events.Log(LogLevel::kInfo, "Kernel already running");
        return true;
    }
    ctx.events.Log(LogLevel::kError, std::format("Wrong response from TCU: {}", bytes::ToHex(*alive)));
    return false;
}

// Decodes the TCU ID's five bytes at PDU offset 4; empty when absent or short.
std::optional<std::string> DecodeTcuId(IEventSink& events, const std::optional<bytes::Bytes>& reply)
{
    constexpr std::size_t kTcuIdOffset = 4;
    constexpr std::size_t kTcuIdSize = 5;
    if (!reply.has_value())
    {
        return std::nullopt;
    }
    if (reply->size() < kTcuIdOffset + kTcuIdSize)
    {
        events.Log(LogLevel::kError, "TCU ID response is too short");
        return std::nullopt;
    }
    std::string decoded = bytes::ToHex(bytes::ByteView{*reply}.subspan(kTcuIdOffset, kTcuIdSize), "{:02X}");
    events.Log(LogLevel::kInfo, std::format("TCU ID: {}", decoded));
    return decoded;
}

// Decodes the CAL ID text from PDU offset 3; empty when absent or short.
std::optional<std::string> DecodeCalId(IEventSink& events, const std::optional<bytes::Bytes>& reply)
{
    constexpr std::size_t kCalIdOffset = 3;
    if (!reply.has_value())
    {
        return std::nullopt;
    }
    if (reply->size() < kCalIdOffset + 1)
    {
        events.Log(LogLevel::kError, "CAL ID response is too short");
        return std::nullopt;
    }
    std::string decoded(reply->begin() + static_cast<std::ptrdiff_t>(kCalIdOffset), reply->end());
    events.Log(LogLevel::kInfo, std::format("CAL ID: {}", decoded));
    return decoded;
}

// Steps 2 and 3: identity mismatches are diagnostic only in legacy. Both
// decoded cleanly compose the legacy ROM id; either missing leaves it unset
// (see the function comment above).
Result<std::optional<std::string>> RequestRomId(const Ctx& ctx)
{
    return Tolerant(ctx, ctx.diagnostic, bytes::Bytes{uds::kSidEcuIdQuery}, Policy(kShortDelay, kResponseTimeout),
                    bytes::Bytes{uds::kSidEcuIdQuery + 0x40}, "TCU ID")
        .and_then(
            [&](const std::optional<bytes::Bytes>& tcu_id)
            {
                const std::optional<std::string> tcu_id_hex = DecodeTcuId(ctx.events, tcu_id);
                return Tolerant(ctx, ctx.diagnostic,
                                bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId},
                                Policy(kShortDelay, kResponseTimeout),
                                bytes::Bytes{uds::kSidVehicleInfoRequest + 0x40, uds::kVehicleInfoPidCalId}, "CAL ID")
                    .transform(
                        [&](const std::optional<bytes::Bytes>& cal_id)
                        {
                            const std::optional<std::string> cal_id_text = DecodeCalId(ctx.events, cal_id);
                            if (tcu_id_hex.has_value() && cal_id_text.has_value())
                            {
                                return std::optional<std::string>{*cal_id_text + "_" + *tcu_id_hex + "_"};
                            }
                            return std::optional<std::string>{};
                        });
            });
}

Status UnlockAndJump(const Ctx& ctx)
{
    // Step 4: enter the extended diagnostic session; mismatch is fatal.
    return Fatal(ctx, ctx.diagnostic, bytes::Bytes{uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic},
                 Policy(kShortDelay, kResponseTimeout), bytes::Bytes{uds::kSessionExtendedDiagnostic},
                 "diagnostic session")
        .and_then(
            [&](const bytes::Bytes&)
            {
                // Step 5: request the four-byte seed. The prefix alone only guarantees
                // the sub-function echo, so guard the seed bytes (PDU offsets 2..5)
                // explicitly.
                return Fatal(
                    ctx, ctx.diagnostic, bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed},
                    Policy(kShortDelay, kResponseTimeout), bytes::Bytes{uds::kSecurityAccessRequestSeed}, "seed");
            })
        .and_then(
            [&](const bytes::Bytes& seed_response) -> Result<bytes::Bytes>
            {
                if (seed_response.size() < 6)
                {
                    return Fail(ErrorKind::kBadResponse, "seed response is too short");
                }
                // Step 6: derive and send the key.
                bytes::Bytes key_request{uds::kSidSecurityAccess, uds::kSecurityAccessSendKey};
                const bytes::Bytes key = SeedKey(bytes::ByteView{seed_response}.subspan(2, 4));
                key_request.insert(key_request.end(), key.begin(), key.end());
                return Fatal(ctx, ctx.diagnostic, key_request, Policy(kShortDelay, kResponseTimeout),
                             bytes::Bytes{uds::kSecurityAccessSendKey}, "key");
            })
        .and_then(
            [&](const bytes::Bytes&)
            {
                // Step 7: legacy logs a bad jump response and continues (its return is
                // commented out), but preserves the 200 ms delay before the read.
                return Tolerant(
                    ctx, ctx.kernel, bytes::Bytes{uds::kSidDiagnosticSessionControl, uds::kSessionProgramming},
                    Policy(kJumpDelay, kResponseTimeout),
                    bytes::Bytes{uds::kSidDiagnosticSessionControl + 0x40, uds::kSessionProgramming}, "kernel jump");
            })
        .and_then(
            [&](const std::optional<bytes::Bytes>& jump)
            {
                if (jump.has_value())
                {
                    ctx.events.Log(LogLevel::kInfo, std::format("kernel jump response: {}", bytes::ToHex(*jump)));
                }
                // Step 8, deliberate divergence 3: build the full four-byte payload in
                // bounds. Legacy reused a six-byte frame and wrote positions 6 and 7
                // beyond its QByteArray, which does not extend under Qt 6.
                return Fatal(ctx, ctx.kernel, bytes::Bytes{uds::kSidRoutineControl, 0x02, 0x02, 0x01},
                             Policy(0ms, kResponseTimeout), bytes::Bytes{0x02, 0x02, 0x03}, "kernel recheck");
            })
        .transform([](const bytes::Bytes&) {});
}

Result<std::optional<std::string>> ConnectBootloader(const Ctx& ctx)
{
    return KernelAlive(ctx).and_then(
        [&](bool alive) -> Result<std::optional<std::string>>
        {
            if (alive)
            {
                // Legacy returns here too, before ever requesting the TCU/CAL
                // ID, so RomId is never set on this path either.
                return std::optional<std::string>{};
            }
            return RequestRomId(ctx).and_then(
                [&](std::optional<std::string> rom_id)
                { return UnlockAndJump(ctx).transform([&] { return std::move(rom_id); }); });
        });
}

Result<bytes::Bytes> ReadRom(const Ctx& ctx, const SubaruTcuHitachiM32rCanPlan& plan, const MemoryRegion& region)
{
    const ICancellationToken& cancellation = ctx.cancellation;
    IEventSink& events = ctx.events;
    // Deliberate divergence 2: legacy subtracted 0x00100000 from a start
    // address of 0, underflowing uint32_t to 0xFFF00000, which is not less than
    // 0x8000 and so walked straight past its own floor clamp. The live path
    // therefore asked the kernel for 0x80000 bytes from 0xFFF00000 and then
    // prepended a further 0x8000 zeros -- a 0x88000-byte image for a 0x80000
    // ROM, which cannot be a valid dump. The port asks for the window the clamp
    // evidently intended: region.length (0x78000) bytes from region.start
    // (0x8000).
    events.Log(LogLevel::kInfo, "Setting dump start & length...");
    bytes::Bytes window_request{uds::kSidRequestDownload, 0x04, 0x33};
    bytes::AppendU24Be(window_request, region.start);
    bytes::AppendU24Be(window_request, region.length);
    if (Result<bytes::Bytes> window = Fatal(ctx, ctx.kernel, window_request, Policy(0ms, kResponseTimeout),
                                            bytes::Bytes{0x20, 0x01, 0x04}, "read window");
        !window.has_value())
    {
        return std::unexpected(window.error());
    }

    events.Log(LogLevel::kInfo, "Start reading ROM, please wait...");
    const std::size_t expected_page_pdu = 1 + plan.page_size;
    bytes::Bytes dumped;
    dumped.reserve(region.length);
    for (std::uint32_t offset = 0; offset < region.length; offset += plan.page_size)
    {
        // Legacy's stopRequested() check at the top of the dump loop.
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during ROM read");
        }
        const std::uint32_t address = region.start + offset;
        bytes::Bytes page_request{uds::kSidReadMemoryChunk};
        bytes::AppendU24Be(page_request, address);
        Result<bytes::Bytes> page =
            Fatal(ctx, ctx.kernel, page_request, Policy(0ms, kResponseTimeout), bytes::ByteView{}, "page read");
        if (!page.has_value())
        {
            return std::unexpected(page.error());
        }
        // Stricter than legacy, which appended whatever followed the five
        // header bytes and so handed back a silently short or long image when
        // a page came back the wrong size. Sizes are PDU sizes: the service
        // id plus the page, the CAN id envelope already stripped.
        if (page->size() != expected_page_pdu)
        {
            return Fail(ErrorKind::kBadResponse, std::format("page read at 0x{:06X} returned {} bytes, expected {}",
                                                             address, page->size(), expected_page_pdu));
        }
        dumped.insert(dumped.end(), page->begin() + 1, page->end());
        events.Progress(static_cast<int>(offset + plan.page_size), static_cast<int>(region.length));
        // Legacy's delay(1): after the page is in hand, before the next request
        // goes out.
        if (const Status slept = ctx.clock.Sleep(kPageDelay, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }

    events.Log(LogLevel::kInfo, "ROM read complete");
    events.Log(LogLevel::kInfo, "Sending stop command...");
    // Legacy logs a bad or missing stop answer and carries on: both of its
    // `return STATUS_ERROR` lines are commented out.
    Result<std::optional<bytes::Bytes>> stop = Tolerant(ctx, ctx.kernel, bytes::Bytes{uds::kSidRequestTransferExit},
                                                        Policy(0ms, kResponseTimeout), bytes::Bytes{0x77}, "dump stop");
    if (!stop.has_value())
    {
        return std::unexpected(stop.error());
    }

    // Deliberate divergence 4: a sized zero buffer. Legacy filled a
    // default-constructed, empty QByteArray through padBytes[i] for i in
    // 0..0x7FFF, which does not extend under Qt 6.
    bytes::Bytes image(region.start, bytes::Byte{0x00});
    const bytes::Bytes decrypted = ssm_protocol::CalculatePayload(dumped, kDecryptTable, ssm_protocol::kStockPairTable);
    image.insert(image.end(), decrypted.begin(), decrypted.end());
    return image;
}

// The one exchange outside the SID+0x40 convention UdsClient enforces: the
// request is 31 02 01 FF FF FF FF and the expected reply starts 31 02 01
// (legacy erase_mem, received.at(4..6)), so it goes through the raw channel and
// keeps its own check. responsePending is therefore not absorbed here.
Status Erase(const Ctx& ctx)
{
    const bytes::Bytes request{uds::kSidRoutineControl, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};
    return CheckCancelled(ctx.cancellation, "cancelled before request")
        .and_then([&] { return ctx.kernel_channel.Send(request, ctx.cancellation); })
        .and_then([&] { return ctx.clock.Sleep(kEraseDelay, ctx.cancellation); })
        .and_then([&] { return ctx.kernel_channel.Receive(kEraseTimeout, ctx.cancellation); })
        .and_then(
            [&](const std::optional<bytes::Bytes>& reply) -> Status
            {
                if (!reply.has_value())
                {
                    return Fail(ErrorKind::kTimeout, "no response from TCU");
                }
                constexpr std::array<bytes::Byte, 3> kEraseReply{0x31, 0x02, 0x01};
                if (reply->size() < kEraseReply.size())
                {
                    return Fail(ErrorKind::kBadResponse, "erase response is too short");
                }
                if (!std::equal(kEraseReply.begin(), kEraseReply.end(), reply->begin()))
                {
                    return Fail(ErrorKind::kBadResponse, "wrong response from TCU");
                }
                return {};
            });
}

Status EraseFlash(const Ctx& ctx)
{
    ctx.events.Log(LogLevel::kInfo, "Erasing TCU ROM...");
    // Deliberate divergence 5, the most dangerous defect in the family: legacy
    // waited 500 ms, read with a 200 ms timeout for a multi-second erase, then
    // indexed received.at(4), at(5) and at(6) with no length guard, and its
    // `return STATUS_ERROR` was commented out. A failed, short or absent erase
    // was logged and reflash proceeded onto possibly-unerased flash. Here any
    // of the three stops the operation before a single 0x34 or 0xB6 frame is
    // sent. The delay, the timeout and the expected bytes are legacy's.
    if (const Status erased = Erase(ctx); !erased.has_value())
    {
        // T4a: scoped to the three divergence-5 shapes (short, wrong content,
        // silent) that all mirror legacy's single, undifferentiated content
        // check -- not to cancellation, where the operator already knows they
        // stopped it, and not to a port/transport error, which is a comms fault
        // rather than TCU-state ambiguity. Legacy had no equivalent of either
        // of those cases at this point.
        if (erased.error().kind == ErrorKind::kBadResponse || erased.error().kind == ErrorKind::kTimeout)
        {
            ctx.events.Log(LogLevel::kError,
                           "Erasing error! Do not panic, do not reset the TCU immediately. The kernel is "
                           "most likely still running and receiving commands!");
        }
        return erased;
    }
    return {};
}

Status ReflashBlock(const Ctx& ctx, const SubaruTcuHitachiM32rCanPlan& plan, const FlashBlock& block,
                    bytes::ByteView encrypted, std::uint32_t& written, std::uint32_t total)
{
    const ICancellationToken& cancellation = ctx.cancellation;
    IEventSink& events = ctx.events;
    const std::uint32_t frame_size = plan.write_frame_size;
    const std::uint32_t frames = block.len / frame_size;
    // Legacy end_addr - start_address: whole frames only.
    const std::uint32_t data_len = frames * frame_size;
    // No test pins this line -- mutation M36, which deletes it, survives by
    // design -- but it must stay: it is the only thing standing between an
    // out-of-range block table and an out-of-bounds read feeding frames that
    // get written to flash hardware. Deleting it re-opens mutation M09 (block
    // range widened past the 11-entry table), which without this guard reads
    // device->fblocks[11] off the end of a constexpr array and then indexes
    // encrypted[0x804000] on a 0x80000-byte vector. On the validated M32R_512KB
    // geometry -- the plan validator pins both the MCU and the 0x80000 image
    // size -- this guard cannot fire; it exists so a future geometry change
    // fails cleanly with InvalidConfig instead of reading out of bounds. Legacy
    // indexed newdata[i + blockaddr] with no such check.
    if (block.start > encrypted.size() || data_len > encrypted.size() - block.start)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("block at 0x{:08X} lies outside the 0x{:X}-byte image", block.start, encrypted.size()));
    }
    events.Log(LogLevel::kInfo, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.len));

    events.Log(LogLevel::kInfo, "Setting flash start & length...");
    bytes::Bytes window_request{uds::kSidRequestDownload, 0x04, 0x33};
    bytes::AppendU24Be(window_request, block.start);
    bytes::AppendU24Be(window_request, data_len);
    if (Result<bytes::Bytes> window =
            Fatal(ctx, ctx.kernel, window_request, Policy(0ms, kResponseTimeout), bytes::ByteView{}, "block window");
        !window.has_value())
    {
        return std::unexpected(window.error());
    }

    for (std::uint32_t frame = 0; frame < frames; ++frame)
    {
        // Legacy's stopRequested() check at the top of the frame loop, which
        // returned STATUS_SUCCESS and so reported a cancelled block as
        // reflashed.
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during block write");
        }
        const std::uint32_t address = block.start + (frame * frame_size);
        bytes::Bytes data_request{uds::kSidWriteMemoryChunk};
        bytes::AppendU24Be(data_request, address);
        const auto offset = static_cast<std::ptrdiff_t>(address);
        data_request.insert(data_request.end(), encrypted.begin() + offset,
                            encrypted.begin() + offset + static_cast<std::ptrdiff_t>(frame_size));
        if (Result<bytes::Bytes> written_frame =
                Fatal(ctx, ctx.kernel, data_request, Policy(kDataFrameDelay, kDataFrameTimeout), bytes::ByteView{},
                      "data frame");
            !written_frame.has_value())
        {
            return std::unexpected(written_frame.error());
        }
        written += frame_size;
        events.Progress(static_cast<int>(written), static_cast<int>(total));
    }

    events.Log(LogLevel::kInfo, "Closing out flashing of this block...");
    // Legacy retries the close up to 20 times; one non-0x77 or absent reply is
    // logged and retried (both `return STATUS_ERROR` lines are commented out),
    // but exhausting the 20 attempts fails the block before the checksum is
    // ever asked for.
    bool closed = false;
    for (int attempt = 0; attempt < kRetryAttempts && !closed; ++attempt)
    {
        Result<std::optional<bytes::Bytes>> close =
            Tolerant(ctx, ctx.kernel, bytes::Bytes{uds::kSidRequestTransferExit}, Policy(0ms, kRetryTimeout),
                     bytes::Bytes{0x77}, "block close");
        if (!close.has_value())
        {
            return std::unexpected(close.error());
        }
        closed = close->has_value();
    }
    if (!closed)
    {
        return Fail(ErrorKind::kBadResponse,
                    std::format("block at 0x{:08X} did not close after {} attempts", block.start, kRetryAttempts));
    }

    if (const Status slept = ctx.clock.Sleep(kChecksumDelay, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }

    events.Log(LogLevel::kInfo, "Verifying checksum...");
    // The 0x71 02 02 answer is the block's only success condition: legacy
    // returns STATUS_SUCCESS from inside this loop and STATUS_ERROR from below
    // it. A short, wrong or absent answer is retried.
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt)
    {
        Result<std::optional<bytes::Bytes>> checksum =
            Tolerant(ctx, ctx.kernel, bytes::Bytes{uds::kSidRoutineControl, 0x02, 0x02, 0x01},
                     Policy(0ms, kRetryTimeout), bytes::Bytes{0x71, 0x02, 0x02}, "block checksum");
        if (!checksum.has_value())
        {
            return std::unexpected(checksum.error());
        }
        if (checksum->has_value())
        {
            events.Log(LogLevel::kInfo, std::format("Block at 0x{:08X} reflash complete.", block.start));
            return {};
        }
    }
    return Fail(ErrorKind::kBadResponse,
                std::format("block at 0x{:08X} checksum failed after {} attempts", block.start, kRetryAttempts));
}

Status WriteRom(const Ctx& ctx, const SubaruTcuHitachiM32rCanPlan& plan, const FlashPlan& flash_plan)
{
    IEventSink& events = ctx.events;
    const FlashDevice *device = FindFlashDevice(flash_plan.McuName());
    if (device == nullptr || device->fblocks == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "Subaru TCU Hitachi M32R CAN flash geometry is invalid");
    }
    if (!flash_plan.Image().has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "Subaru TCU Hitachi M32R CAN write plan carries no ROM image");
    }

    // Legacy encrypts the whole image before the first frame leaves and then
    // indexes it by absolute flash address (newdata[i + blockaddr] over
    // fblocks[0].start == 0).
    const bytes::Bytes& image = *flash_plan.Image();
    const bytes::Bytes encrypted = ssm_protocol::CalculatePayload(image, kEncryptTable, ssm_protocol::kStockPairTable);
    // A second defensive guard, same treatment as the one in reflash_block above:
    // no test pins this line, and it is unreachable on a correct build --
    // calculatePayload only ever truncates its output to a multiple of 4, and
    // encrypted is built from image in the line just above, so the sizes cannot
    // diverge. It stays because a future change to calculatePayload's truncation
    // behaviour must fail cleanly here instead of letting reflash_block index
    // encrypted with offsets computed from the (now wrong) image size.
    if (encrypted.size() != image.size())
    {
        return Fail(ErrorKind::kInternal, "encrypted image size does not match the ROM image");
    }

    const unsigned blocks = std::min<unsigned>(device->numblocks, static_cast<unsigned>(kBlockModified.size()));
    std::uint32_t total = 0;
    for (unsigned blockno = 0; blockno < blocks; ++blockno)
    {
        if (kBlockModified.at(blockno))
        {
            total += device->fblocks[blockno].len;
        }
    }
    if (total == 0)
    {
        events.Log(LogLevel::kInfo, "*** No blocks require flash! ***");
        return {};
    }

    events.Log(LogLevel::kInfo, "--- erasing TCU flash memory ---");
    if (const Status erased = EraseFlash(ctx); !erased.has_value())
    {
        return erased;
    }

    events.Log(LogLevel::kInfo, "--- start writing ROM file to ECU flash memory ---");
    std::uint32_t written = 0;
    for (unsigned blockno = 0; blockno < blocks; ++blockno)
    {
        if (!kBlockModified.at(blockno))
        {
            continue;
        }
        if (const Status flashed = ReflashBlock(ctx, plan, device->fblocks[blockno], encrypted, written, total);
            !flashed.has_value())
        {
            events.Log(LogLevel::kError, std::format("Block {} reflash failed.", blockno));
            return flashed;
        }
    }
    return {};
}

Result<FlashExecutionResult> ExecuteTransfer(const FlashPlan& plan, const Ctx& ctx,
                                             const SubaruTcuHitachiM32rCanPlan& parameters,
                                             std::optional<std::string> rom_id)
{
    if (plan.Operation() == FlashOperation::kRead)
    {
        Result<bytes::Bytes> image = ReadRom(ctx, parameters, plan.TransferRegion());
        if (!image.has_value())
        {
            return std::unexpected(image.error());
        }
        return FlashExecutionResult{
            .operation = FlashOperation::kRead,
            .read_bytes = std::move(*image),
            .rom_id = std::move(rom_id),
        };
    }
    if (plan.Operation() != FlashOperation::kWrite)
    {
        // M2: fail-closed rather than fail-open in shape. Every operation
        // that reaches this point other than Read used to fall straight into
        // write_rom -- safe only because execute() validates the plan
        // immediately before calling this function. TestWrite is already
        // rejected earlier by validate_subaru_tcu_hitachi_m32r_can_plan
        // (deliberate divergence 1), which both transport_setup() and
        // execute() call on every plan before this function ever runs, so
        // this branch cannot currently be reached through
        // SubaruTcuHitachiM32rCanExecutor::execute() -- the same "no test can
        // pin it" shape as the reflash_block/write_rom guards below. It stays
        // because this function's own shape should not perform a real write
        // for an operation it was not asked to perform, independent of
        // whether the upstream validation is ever bypassed.
        return Fail(ErrorKind::kUnsupported, "Subaru TCU Hitachi M32R CAN supports read and write only");
    }
    // Deliberate divergence 1: TestWrite never reaches here. It is rejected
    // by validate_subaru_tcu_hitachi_m32r_can_plan
    // (subaru_tcu_hitachi_m32r_can_plan.cpp), which both transport_setup()
    // and execute() call on every plan before this function runs. This
    // family has exactly two independent TestWrite checks -- that validator
    // check, and the plan-builder check in the same file -- not four;
    // execute()'s call into the validator and the guard just above are both
    // consumers of the validator check, not additional checks of their own.
    // Legacy's reflash_block took a test_write_arg and never read it, so
    // "test write" performed a real erase and a real flash write.
    if (const Status flashed = WriteRom(ctx, parameters, plan); !flashed.has_value())
    {
        return std::unexpected(flashed.error());
    }
    return FlashExecutionResult{
        .operation = FlashOperation::kWrite,
        .read_bytes = std::nullopt,
        // Legacy only assigns ecuCalDef->RomId when cmd_type == "read"; the
        // decoded id is discarded here too, even though connect_bootloader()
        // decodes it on every connect regardless of operation.
        .rom_id = std::nullopt,
    };
}

} // namespace

Result<Iso15765Config> SubaruTcuHitachiM32rCanExecutor::Setup(const SubaruTcuHitachiM32rCanPlan& family) const
{
    return Iso15765ConfigFrom(family);
}

Result<FlashExecutionResult> SubaruTcuHitachiM32rCanExecutor::Run(const SubaruTcuHitachiM32rCanPlan& parameters,
                                                                  const FlashPlan& plan, ICanFlashTransport& transport,
                                                                  IClock& clock, const ICancellationToken& cancellation,
                                                                  IEventSink& events)
{
    // Identity, session, seed and key go to 0x7E0; everything else to the
    // plan's request id. Both pairs share the one transport and are used
    // strictly in sequence.
    CanFlashUdsChannel diagnostic_channel(transport, kDiagnosticRequestId, parameters.response_id);
    uds::UdsClient diagnostic(diagnostic_channel, clock, events);
    CanFlashUdsChannel kernel_channel(transport, parameters.request_id, parameters.response_id);
    uds::UdsClient kernel(kernel_channel, clock, events);
    const Ctx ctx{cancellation, events, clock, diagnostic, kernel, kernel_channel};

    return ConnectBootloader(ctx).and_then([&](std::optional<std::string> rom_id)
                                           { return ExecuteTransfer(plan, ctx, parameters, std::move(rom_id)); });
}

} // namespace fastecu::flash
