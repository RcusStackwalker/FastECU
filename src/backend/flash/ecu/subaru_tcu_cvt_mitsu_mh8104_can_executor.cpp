#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/bootloader_operation.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_plan.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can_operation.cpp
// it was transcribed from.
//
// This family's defining, deliberate-to-preserve quirk (see Step 1 of the
// implementation brief): every one of connect_bootloader's, read_mem's, and
// reflash_block's response-content checks after the initial kernel-alive
// probe is followed by a commented-out `// return STATUS_ERROR;` in legacy
// -- this family tolerates ANY ECU response content and only a genuine
// transport-level failure (timeout/disconnect/cancellation) between
// exchanges can stop it.
//
// Because legacy tolerates a reply carrying the WRONG service id just as
// readily as a right-but-wrong-content one (its own checks index raw bytes
// positionally, never validating the leading service byte against the sent
// PDU's own SID+0x40 convention before deciding whether to proceed), this
// executor deliberately does NOT route any exchange through
// uds::UdsClient::request() -- UdsClient enforces exactly that SID+0x40
// match as a precondition for success, which would silently make this port
// STRICTER than legacy for any exchange whose scripted/real reply carries
// an unexpected service id. Every exchange instead goes through
// uds::IUdsChannel::send()/receive() directly, and content is checked (for
// logging only, never for fatality) with bounds-safe helpers below -- never
// legacy's raw, unguarded `.at()` indexing.
namespace fastecu::flash
{
namespace
{
using namespace bytes::literals;
using namespace std::chrono_literals;
using bytes::ComposeBe;
using bytes::U24;

// Seed key (legacy generate_seed_key) -- identical to the sibling MH8111
// family's table, confirmed by direct byte-for-byte comparison against both
// legacy sources, not assumed from similarity. Encrypt (write payload, legacy
// encrypt_payload) and decrypt (read payload, legacy decrypt_payload) tables:
// same finding. All three (Task 6) are factored into
// subaru_tcu_cvt_mitsu_can_common.h/.cpp.

// Legacy read_mem hardcodes start_addr = 0x8000, length = 0x78000
// unconditionally ("hack for testing"). Unlike MH8111 (whose read window and
// sole flashed block do NOT overlap), this family's write window is IDENTICAL
// -- both are kFlashBlocksMH8104[3] (kernelmemorymodels.h).
constexpr MemoryRegion kReadRegion{0x8000, 0x78000};
constexpr MemoryRegion kWriteRegion{0x8000, 0x78000};

// Session ids in ISO 14229-1's 0x40-0x5F vehicle-manufacturer-specific
// band -- no standard meaning.
constexpr bytes::Byte kSessionBootload = 0x43;
constexpr bytes::Byte kSessionKernelJump = 0x42;

// Bounds-safe prefix match: true only if `reply` is at least as long as
// `expected` and every byte matches. Used throughout in place of legacy's
// raw `.at(N)` indexing, which has no length guard anywhere in this file
// (not just the kernel-alive probe -- see the file header comment).
bool Matches(bytes::ByteView reply, std::initializer_list<bytes::Byte> expected)
{
    return reply.size() >= expected.size() && std::equal(expected.begin(), expected.end(), reply.begin());
}

struct Ctx
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    IClock& clock;
    uds::IUdsChannel& channel;
};

Error ReportExchangeFailure(Ctx& ctx, const Error& failure, std::string_view rejection_prefix,
                            std::string_view operation)
{
    if (failure.kind == ErrorKind::kCancelled)
    {
        ctx.events.Log(LogLevel::kWarning, std::format("Cancelled by operator during {} -- this is not an ECU "
                                                       "rejection. The request may already have reached the ECU "
                                                       "and still be running there; check the unit before "
                                                       "power-cycling it.",
                                                       operation));
        return failure;
    }
    LogError(ctx, std::format("{}{}", rejection_prefix, failure.detail));
    return failure;
}

// Sends `pdu` and waits up to `timeout` for a reply. A hard
// transport-level failure (disconnect/cancel/malformed envelope) is always
// propagated as a Result failure. A read that reaches its deadline with
// nothing received comes back as a successful EMPTY optional (see
// IUdsChannel::receive's own contract) -- callers below decide what an
// absent reply means for their own exchange.
Result<std::optional<bytes::Bytes>> Exchange(Ctx& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout)
{
    return ctx.channel.Send(pdu, ctx.cancellation)
        .and_then([&] { return ctx.channel.Receive(timeout, ctx.cancellation); });
}

// A single send + read whose reply CONTENT is never checked for fatality --
// every response check in legacy after the kernel-alive probe is followed
// by a commented-out `// return STATUS_ERROR;`, so a present-but-wrong
// reply only gets logged by the caller, never fails the exchange. An
// entirely absent reply (nothing within `timeout`) is the one thing
// mapped to a defined, FATAL Timeout here: legacy's own `.at()` calls on an
// empty `received` are unguarded out-of-bounds reads at exactly this point
// (every single-shot exchange in this file shares that same missing length
// guard, not just the kernel-alive probe -- see the file header comment),
// so this is the safe, defined analogue of that crash, not a new fatality
// decision invented for the port.
Result<bytes::Bytes> SingleShot(Ctx& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout,
                                std::string_view label)
{
    Result<std::optional<bytes::Bytes>> reply = Exchange(ctx, pdu, timeout);
    if (!reply.has_value())
    {
        return std::unexpected(ReportExchangeFailure(ctx, reply.error(), "Wrong response from TCU: ", label));
    }
    if (!reply->has_value())
    {
        LogError(ctx, std::format("No response from TCU to {}", label));
        return Fail(ErrorKind::kTimeout, std::format("no response to {}", label));
    }
    return std::move(**reply);
}

// Retries up to `attempts` times, content-blind: legacy's own condition is `if
// (received != "")`, so ANY reply -- right SID, wrong SID, negative, whatever
// -- stops the retry early. A hard transport failure at any attempt is
// immediately fatal (never retried). If every attempt times out, this still
// returns success (an empty optional) so the caller proceeds regardless --
// legacy's own post-loop code never checks `connected` for the TCU ID/CAL ID
// queries, and its post-loop content check is itself commented out for
// reflash_block's setup/close/checksum siblings.
Result<std::optional<bytes::Bytes>> RetryUntilAnyReply(Ctx& ctx, bytes::ByteView pdu, int attempts,
                                                       std::chrono::milliseconds timeout)
{
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        Result<std::optional<bytes::Bytes>> reply = Exchange(ctx, pdu, timeout);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        if (reply->has_value())
        {
            return reply;
        }
    }
    return std::optional<bytes::Bytes>{};
}

// Kernel-alive probe: 0x31/0x02/0x02/0x01, single send+read, no retry. Legacy
// indexes `received.at(4)` through `.at(7)` with NO length guard before this
// comparison -- unlike every sibling family's own alive-probe check. This is
// also the one exchange in the whole function where an ABSENT reply is the
// routine case (the TCU's stock firmware, not yet in the diagnostic kernel, is
// not expected to answer this PDU at all) rather than a sign of a broken
// connection -- so both an empty reply and a present-but-wrong one report "not
// running"; only an exact match reports true, and only a genuine
// transport-level failure (Disconnected/Cancelled/malformed envelope) is fatal
// here. uds::Payload() bounds-checks the reply bytes past the service id
// instead of raw-indexing off the end of a short buffer, so the port is
// structurally incapable of the legacy UB here, not merely avoiding it by
// convention (see the implementation brief's Step 7).
Result<bool> KernelAlreadyRunning(Ctx& ctx)
{
    LogInfo(ctx, "Checking if kernel is already running...");
    Result<std::optional<bytes::Bytes>> alive_probe =
        Exchange(ctx, bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStop, 0x02, 0x01}, 200ms);
    if (!alive_probe.has_value())
    {
        return std::unexpected(alive_probe.error());
    }
    if (!alive_probe->has_value())
    {
        return false;
    }
    const bytes::ByteView reply = **alive_probe;
    if (reply.empty() || reply[0] != 0x71)
    {
        return false;
    }
    const bytes::ByteView rest = uds::Payload(reply);
    return rest.size() >= 3 && rest[0] == uds::kRoutineControlStop && rest[1] == 0x02 && rest[2] == 0x03;
}

// Shared shape of the TCU ID / CAL ID init retries: retried up to `attempts`
// times, content-blind, proceeds regardless even if every attempt times out.
// `success_prefix` and `no_response_label` carry each site's own legacy log
// wording verbatim (the two success messages are not the same string in legacy
// either).
Result<std::optional<bytes::Bytes>> RetryInitStep(Ctx& ctx, bytes::ByteView pdu, int attempts,
                                                  std::chrono::milliseconds timeout, std::string_view success_prefix,
                                                  std::string_view no_response_label)
{
    Result<std::optional<bytes::Bytes>> reply = RetryUntilAnyReply(ctx, pdu, attempts, timeout);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (reply->has_value())
    {
        LogInfo(ctx, std::format("{}{}", success_prefix, bytes::ToHex(**reply)));
    }
    else
    {
        LogInfo(ctx, std::format("No response to {} after {} attempts -- proceeding regardless", no_response_label,
                                 attempts));
    }
    return reply;
}

// Shared shape of every content-blind single-shot exchange below (session
// init, seed, seed key, jump): a mismatch only logs `mismatch_message` --
// legacy's own `// return STATUS_ERROR;` on these checks is commented out --
// and the reply is returned either way for the caller to use.
Result<bytes::Bytes> SingleShotLogged(Ctx& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout,
                                      std::string_view label, std::initializer_list<bytes::Byte> expect,
                                      std::string_view mismatch_message)
{
    Result<bytes::Bytes> reply = SingleShot(ctx, pdu, timeout, label);
    if (!reply.has_value())
    {
        return reply;
    }
    if (!Matches(*reply, expect))
    {
        LogError(ctx, mismatch_message);
    }
    return reply;
}

// Alive re-check content test. Legacy's own condition uses `&&` where every
// sibling exchange in this file uses `||` -- transcribed literally: true
// (kernel verified running) only when the reply's SID, format id,
// address-format id, AND length-format id ALL simultaneously differ from
// 0x74/0x20/0x01/0x04, which in practice almost never happens for a well-formed
// reply.
bool KernelVerifiedRunning(const bytes::Bytes& alive_recheck)
{
    return alive_recheck.size() >= 4 && alive_recheck[0] != 0x74 && alive_recheck[1] != 0x20 &&
           alive_recheck[2] != 0x01 && alive_recheck[3] != 0x04;
}

Status ConnectBootloader(Ctx& ctx)
{
    Result<bool> running = KernelAlreadyRunning(ctx);
    if (!running.has_value())
    {
        return std::unexpected(running.error());
    }
    if (*running)
    {
        LogInfo(ctx, "Kernel already running");
        return {};
    }

    LogInfo(ctx, "Trying TCU Init...");
    if (Result<std::optional<bytes::Bytes>> tcu_id =
            RetryInitStep(ctx, bytes::Bytes{uds::kSidEcuIdQuery}, 6, 200ms, "Init Success: ", "TCU Init");
        !tcu_id.has_value())
    {
        return std::unexpected(tcu_id.error());
    }

    LogInfo(ctx, "Trying 0x09 0x04...");
    if (Result<std::optional<bytes::Bytes>> cal_id =
            RetryInitStep(ctx, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId}, 6, 200ms,
                          "Init Success: TCU ID = ", "0x09 0x04");
        !cal_id.has_value())
    {
        return std::unexpected(cal_id.error());
    }

    LogInfo(ctx, "Initializing bootloader...");
    if (Result<bytes::Bytes> session =
            SingleShotLogged(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionBootload}, 200ms,
                             "session init", {0x50, kSessionBootload}, "Failed to initialise bootloader");
        !session.has_value())
    {
        return std::unexpected(session.error());
    }

    LogInfo(ctx, "Starting seed request...");
    Result<bytes::Bytes> seed_reply =
        SingleShotLogged(ctx, bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, 200ms,
                         "the seed request", {0x67, uds::kSecurityAccessRequestSeed}, "Bad response to seed request");
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    LogInfo(ctx, "Seed request ok");
    // Legacy indexes received.at(6..9) -- the 4 bytes after the subfunction
    // byte of the stripped PDU -- with no length guard. uds::Payload() strips
    // the service id bounds-safely; any seed byte past what the reply actually
    // carries defaults to 0x00 rather than reading out of bounds, matching this
    // family's total content tolerance (a short/wrong reply still produces SOME
    // seed key, never a crash).
    bytes::Bytes seed(4, 0x00);
    {
        const bytes::ByteView after_sid = uds::Payload(*seed_reply); // [subfunction, seed x4]
        for (std::size_t i = 0; i < 4 && i + 1 < after_sid.size(); ++i)
        {
            seed[i] = after_sid[i + 1];
        }
    }
    const bytes::Bytes key = TcuCvtMitsuSeedKey(seed);

    // Seed key 0x27/0x02: single-shot, content-blind.
    LogInfo(ctx, "Sending seed key...");
    bytes::Bytes key_request{uds::kSidSecurityAccess, uds::kSecurityAccessSendKey};
    key_request.insert(key_request.end(), key.begin(), key.end());
    if (Result<bytes::Bytes> key_reply =
            SingleShotLogged(ctx, key_request, 200ms, "the seed key", {0x67, uds::kSecurityAccessSendKey},
                             "Bad response to seed request");
        !key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    LogInfo(ctx, "Seed key ok");

    LogInfo(ctx, "Jumping to onboad kernel...");
    if (Result<bytes::Bytes> jump_reply = SingleShotLogged(
            ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionKernelJump}, 200ms, "the kernel jump",
            {0x50, kSessionKernelJump}, "Bad response to jumping to onboard kernel");
        !jump_reply.has_value())
    {
        return std::unexpected(jump_reply.error());
    }
    LogInfo(ctx, "Jump to kernel ok");

    // Alive re-check: sent 0x34/0x04/0x33/0x00/0x00/0x00/0x08/0x00/0x00,
    // single-shot. There is no `else` branch either way -- connect_bootloader
    // falls through to "Test script complete" and returns success regardless of
    // kernel_verified_running()'s outcome.
    LogInfo(ctx, "Checking if jump successful and kernel alive...");
    Result<bytes::Bytes> alive_recheck =
        SingleShot(ctx, bytes::Bytes{uds::kSidRequestDownload, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}, 200ms,
                   "the alive re-check");
    if (!alive_recheck.has_value())
    {
        return std::unexpected(alive_recheck.error());
    }
    if (KernelVerifiedRunning(*alive_recheck))
    {
        LogInfo(ctx, "Kernel verified to be running");
    }

    LogInfo(ctx, "Test script complete");
    return {};
}

Result<bytes::Bytes> DumpFlashRange(Ctx& ctx, PhaseReporter& progress)
{
    constexpr std::uint32_t kPageSize = 0x100;

    // "Setting dump start & length...": single-shot, content-blind (its `//
    // return STATUS_ERROR;` is commented out). The trailing 6 bytes are a
    // hardcoded literal, NOT derived from kReadRegion.start/length, matching
    // the sibling MH8111 family's own dump-setup exchange exactly.
    LogInfo(ctx, "Setting dump start & length...");
    Result<bytes::Bytes> setup =
        SingleShot(ctx, bytes::Bytes{uds::kSidRequestUpload, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}, 200ms,
                   "dump start & length setup");
    if (!setup.has_value())
    {
        return std::unexpected(setup.error());
    }
    if (!Matches(*setup, {0x75, 0x20, 0x01, 0x01}))
    {
        LogError(ctx, "Bad response to setting dump start & length");
    }

    LogInfo(ctx, "Start reading ROM, please wait...");
    bytes::Bytes rom;
    rom.reserve(kReadRegion.length);
    for (std::uint32_t addr = kReadRegion.start; addr < kReadRegion.start + kReadRegion.length; addr += kPageSize)
    {
        // Legacy stopRequested(), top of loop.
        if (ctx.cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "read cancelled");
        }

        // 0xB7 dump request: single-shot per chunk, NO retry, content-blind
        // (its `// return STATUS_ERROR;` for "Page data request failed!" is
        // commented out).
        Result<bytes::Bytes> chunk = SingleShot(ctx, ComposeBe(uds::kSidReadMemoryChunk, U24{addr}), 200ms,
                                                std::format("the flash read at 0x{:x}", addr));
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        if (!Matches(*chunk, {0xF7}))
        {
            LogError(ctx, "Page data request failed!");
        }
        const bytes::Bytes decrypted = TcuCvtMitsuDecryptPage(uds::Payload(*chunk));
        rom.insert(rom.end(), decrypted.begin(), decrypted.end());
        progress.Update(static_cast<int>(addr + kPageSize - kReadRegion.start));
    }

    LogInfo(ctx, "ROM read complete");

    // "Sending stop command...": retried up to 6 times, content-blind, and
    // OUTCOME-blind too -- legacy has no `if (try_count == 6) return ERROR`
    // after this loop at all (unlike erase_mem's/reflash_block's own retry
    // loops, whose post-loop check is merely commented out rather than absent),
    // so read_mem proceeds to decrypt and return success regardless of whether
    // the TCU ever acknowledged stop.
    LogInfo(ctx, "Sending stop command...");
    if (Result<std::optional<bytes::Bytes>> stop =
            RetryUntilAnyReply(ctx, bytes::Bytes{uds::kSidRequestTransferExit}, 6, 200ms);
        !stop.has_value())
    {
        return std::unexpected(stop.error());
    }

    // Legacy pads the unread low 0x8000 region with 0xFF, the same padding
    // shape as the sibling MH8111 family.
    rom.insert(rom.begin(), kReadRegion.start, 0xFF);
    return rom;
}

// A single send, NOT a retry loop (unlike the sibling MH8111 family's own
// erase, which this port must not confuse this one with -- see the
// implementation brief's warning on this exact point).
Status EraseMemory(Ctx& ctx)
{
    LogInfo(ctx, "Erasing TCU ROM...");
    return ctx.channel
        .Send(bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff},
              ctx.cancellation)
        // Legacy delay(8000) before reading the erase acknowledgement -- the
        // TCU needs this long to actually erase flash before it can answer.
        // Modeled via ctx.clock.sleep() so tests assert the timing parameter
        // instead of literally waiting 8 seconds.
        .and_then([&] { return ctx.clock.Sleep(8000ms, ctx.cancellation); })
        .and_then([&] { return ctx.channel.Receive(200ms, ctx.cancellation); })
        .and_then(
            [&](const std::optional<bytes::Bytes>& reply)
            {
                // Legacy delay(5000) unconditionally after reading.
                return ctx.clock.Sleep(5000ms, ctx.cancellation)
                    .and_then(
                        [&]() -> Status
                        {
                            if (!reply.has_value())
                            {
                                LogError(ctx, "No response from TCU to the erase command");
                                return Fail(ErrorKind::kTimeout, "no response to the erase command");
                            }
                            // Content check is non-fatal -- `// return STATUS_ERROR;`
                            // is commented out.
                            if (!Matches(*reply, {0x71, uds::kRoutineControlStart, 0x02}))
                            {
                                LogError(ctx, "Erasing error! Do not panic, do not reset the TCU immediately. The "
                                              "kernel is most likely still running and receiving commands!");
                            }
                            return {};
                        });
            });
}

// Called once (this family flashes exactly one block, kWriteRegion /
// kFlashBlocksMH8104[3]).
Status UnlockAndReflashBlock(Ctx& ctx, bytes::ByteView image, PhaseReporter& progress)
{
    const bytes::ByteView block_plain = image.subspan(kWriteRegion.start, kWriteRegion.length);
    constexpr std::uint32_t kChunkSize = 128;

    // "Setting flash start & length...": retried up to 6 times, content-blind
    // for retry purposes (`if (received != "")`) and non-fatal even after
    // exhausting every attempt (its `// return STATUS_ERROR;` is commented
    // out). Unlike MH8111's own setup packet (whose declared data_len is halved
    // by a legacy arithmetic bug -- maxblocks computed with a *128 unit but the
    // write loop chunks at *256), MH8104's maxblocks and data_len both use the
    // SAME 128-byte unit (`maxblocks = pl_len / 128; data_len = maxblocks *
    // 128`), and pl_len (0x78000) divides evenly by 128, so data_len == pl_len
    // == kWriteRegion.length exactly -- confirmed directly against the source,
    // not assumed from the sibling family's own halving quirk.
    LogInfo(ctx, "Setting flash start & length...");
    Result<std::optional<bytes::Bytes>> setup = RetryUntilAnyReply(
        ctx, ComposeBe(uds::kSidRequestDownload, 0x04_b, 0x33_b, U24{kWriteRegion.start}, U24{kWriteRegion.length}), 6,
        200ms);
    if (!setup.has_value())
    {
        return std::unexpected(setup.error());
    }
    if (!setup->has_value() || !Matches(**setup, {0x74}))
    {
        LogError(ctx, "No or bad response received");
    }

    const bytes::Bytes encrypted = TcuCvtMitsuEncryptRom(block_plain);

    for (std::uint32_t offset = 0; offset < kWriteRegion.length; offset += kChunkSize)
    {
        // Legacy's stopRequested() check returns 0 (success) to the caller on
        // cancellation -- the same bug Tasks 1 and 3 already disclosed and
        // diverged from. Not reproduced here: a cancellation mid-write is
        // reported as Cancelled, never success.
        if (ctx.cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "write cancelled");
        }

        const std::uint32_t addr = kWriteRegion.start + offset;
        const bytes::ByteView chunk_data = bytes::ByteView(encrypted).subspan(offset, kChunkSize);
        // Legacy reads but never inspects the per-chunk reply at all
        // (receive_timeout=500) -- not even for presence: only a
        // transport-level failure from the read itself fails this exchange,
        // unlike every single-shot exchange above.
        if (const Status sent =
                ctx.channel.Send(ComposeBe(uds::kSidWriteMemoryChunk, U24{addr}, chunk_data), ctx.cancellation);
            !sent.has_value())
        {
            return sent;
        }
        if (Result<std::optional<bytes::Bytes>> reply = ctx.channel.Receive(500ms, ctx.cancellation);
            !reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        progress.Update(static_cast<int>(offset + kChunkSize));
    }

    // Closing: retried up to 6 times, content-blind, non-fatal even after
    // exhausting every attempt (its `// return STATUS_ERROR;` is commented
    // out).
    LogInfo(ctx, "Closing out Flashing of this block...");
    Result<std::optional<bytes::Bytes>> closed =
        RetryUntilAnyReply(ctx, bytes::Bytes{uds::kSidRequestTransferExit}, 6, 200ms);
    if (!closed.has_value())
    {
        return std::unexpected(closed.error());
    }
    if (!closed->has_value() || !Matches(**closed, {0x77}))
    {
        LogError(ctx, "No or bad response received");
    }

    // Legacy delay(100) between close and checksum.
    if (const Status slept = ctx.clock.Sleep(100ms, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    // "Verifying checksum...": retried up to 6 times, content-blind, non-fatal
    // even after exhausting every attempt (its `// return STATUS_ERROR;` is
    // commented out).
    LogInfo(ctx, "Verifying checksum...");
    Result<std::optional<bytes::Bytes>> checksum = RetryUntilAnyReply(
        ctx, bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, 0x02, 0x02, 0x01}, 6, 200ms);
    if (!checksum.has_value())
    {
        return std::unexpected(checksum.error());
    }
    if (!checksum->has_value() || !Matches(**checksum, {0x71, uds::kRoutineControlStart, 0x02}))
    {
        LogError(ctx, "No or bad response received");
    }
    else
    {
        LogInfo(ctx, "Checksum verified...");
    }

    return {};
}

} // namespace

Result<Iso15765Config> SubaruTcuCvtMitsuMh8104CanExecutor::Setup(const SubaruTcuCvtMitsuMh8104CanPlan& family) const
{
    return Iso15765ConfigFrom(family);
}

Result<FlashExecutionResult> SubaruTcuCvtMitsuMh8104CanExecutor::Run(const SubaruTcuCvtMitsuMh8104CanPlan& family,
                                                                     const FlashPlan& plan,
                                                                     ICanFlashTransport& transport, IClock& clock,
                                                                     const ICancellationToken& cancellation,
                                                                     IEventSink& events)
{
    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    Ctx ctx{cancellation, events, clock, channel};
    return RunBootloaderOperation(plan, "Subaru TCU CVT Mitsu MH8104", ctx, ConnectBootloader, DumpFlashRange,
                                  EraseMemory, UnlockAndReflashBlock);
}

} // namespace fastecu::flash
