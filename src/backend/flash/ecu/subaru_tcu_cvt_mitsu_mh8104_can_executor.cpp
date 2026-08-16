#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/backend/flash/can_flash_uds_channel.h"
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
using bytes::composeBe;
using bytes::u24;

// Seed key (legacy generate_seed_key, lines 903-907) -- identical to the
// sibling MH8111 family's table, confirmed by direct byte-for-byte
// comparison against both legacy sources, not assumed from similarity.
// Encrypt (write payload, legacy encrypt_payload lines 935-936) and decrypt
// (read payload, legacy decrypt_payload lines 953-954) tables: same finding.
// All three (Task 6) are factored into
// subaru_tcu_cvt_mitsu_can_common.h/.cpp.
// Shared index-transformation table (legacy lines 909-913), identical to
// every family in this wave and wave-1 Hitachi K-Line.
constexpr std::array<std::uint8_t, 32> kIndexTransformation{
    0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
    0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

// Legacy read_mem hardcodes start_addr = 0x8000, length = 0x78000
// unconditionally (lines 364-366, "hack for testing"). Unlike MH8111 (whose
// read window and sole flashed block do NOT overlap), this family's write
// window is IDENTICAL -- both are fblocks_MH8104[3] (kernelmemorymodels.h).
constexpr MemoryRegion kReadRegion{0x8000, 0x78000};
constexpr MemoryRegion kWriteRegion{0x8000, 0x78000};

bytes::Bytes seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, tcuCvtMitsuSeedKeyTable().data(),
                                         kIndexTransformation.data());
}

bytes::Bytes encrypt_rom(bytes::ByteView image)
{
    return SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()),
                                         tcuCvtMitsuEncryptTable().data(),
                                         kIndexTransformation.data());
}

bytes::Bytes decrypt_page(bytes::ByteView page)
{
    return SsmProtocol::calculatePayload(page, static_cast<std::uint32_t>(page.size()),
                                         tcuCvtMitsuDecryptTable().data(),
                                         kIndexTransformation.data());
}

// Bounds-safe prefix match: true only if `reply` is at least as long as
// `expected` and every byte matches. Used throughout in place of legacy's
// raw `.at(N)` indexing, which has no length guard anywhere in this file
// (not just the kernel-alive probe -- see the file header comment).
bool matches(bytes::ByteView reply, std::initializer_list<bytes::Byte> expected)
{
    return reply.size() >= expected.size() &&
           std::equal(expected.begin(), expected.end(), reply.begin());
}

struct Ctx
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    IClock& clock;
    uds::IUdsChannel& channel;
};

class PhaseReporter
{
  public:
    PhaseReporter(IEventSink& events, std::string_view name, int index, int count, int total)
        : events_(events), name_(name), index_(index), count_(count), total_(total)
    {
        emit(0);
    }

    void update(int done)
    {
        const int maximum_incomplete = std::max(0, total_ - 1);
        const int next = std::clamp(done, last_, maximum_incomplete);
        if (next != last_)
        {
            emit(next);
        }
    }

    void complete()
    {
        if (last_ != total_)
        {
            emit(total_);
        }
    }

  private:
    void emit(int done)
    {
        last_ = done;
        events_.phase_progress({name_, index_, count_, done, total_});
    }

    IEventSink& events_;
    std::string_view name_;
    int index_;
    int count_;
    int total_;
    int last_ = 0;
};

class PhaseSequence
{
  public:
    PhaseSequence(IEventSink& events, int count) : events_(events), count_(count)
    {
    }

    PhaseReporter start(std::string_view name, int total)
    {
        return PhaseReporter(events_, name, ++index_, count_, total);
    }

  private:
    IEventSink& events_;
    int count_;
    int index_ = 0;
};

void info(Ctx& ctx, std::string_view message)
{
    ctx.events.log(LogLevel::Info, message);
}

void error(Ctx& ctx, std::string_view message)
{
    ctx.events.log(LogLevel::Error, message);
}

Error report_exchange_failure(Ctx& ctx, const Error& failure, std::string_view rejection_prefix,
                              std::string_view operation)
{
    if (failure.kind == ErrorKind::Cancelled)
    {
        ctx.events.log(LogLevel::Warning,
                       std::format("Cancelled by operator during {} -- this is not an ECU "
                                   "rejection. The request may already have reached the ECU "
                                   "and still be running there; check the unit before "
                                   "power-cycling it.",
                                   operation));
        return failure;
    }
    error(ctx, std::format("{}{}", rejection_prefix, failure.detail));
    return failure;
}

// Sends `pdu` and waits up to `timeout_ms` for a reply. A hard
// transport-level failure (disconnect/cancel/malformed envelope) is always
// propagated as a Result failure. A read that reaches its deadline with
// nothing received comes back as a successful EMPTY optional (see
// IUdsChannel::receive's own contract) -- callers below decide what an
// absent reply means for their own exchange.
Result<std::optional<bytes::Bytes>> exchange(Ctx& ctx, bytes::ByteView pdu, int timeout_ms)
{
    if (const Status sent = ctx.channel.send(pdu, ctx.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    return ctx.channel.receive(timeout_ms, ctx.cancellation);
}

// A single send + read whose reply CONTENT is never checked for fatality --
// every response check in legacy after the kernel-alive probe is followed
// by a commented-out `// return STATUS_ERROR;`, so a present-but-wrong
// reply only gets logged by the caller, never fails the exchange. An
// entirely absent reply (nothing within `timeout_ms`) is the one thing
// mapped to a defined, FATAL Timeout here: legacy's own `.at()` calls on an
// empty `received` are unguarded out-of-bounds reads at exactly this point
// (every single-shot exchange in this file shares that same missing length
// guard, not just the kernel-alive probe -- see the file header comment),
// so this is the safe, defined analogue of that crash, not a new fatality
// decision invented for the port.
Result<bytes::Bytes> single_shot(Ctx& ctx, bytes::ByteView pdu, int timeout_ms,
                                 std::string_view label)
{
    Result<std::optional<bytes::Bytes>> reply = exchange(ctx, pdu, timeout_ms);
    if (!reply.has_value())
    {
        return std::unexpected(
            report_exchange_failure(ctx, reply.error(), "Wrong response from TCU: ", label));
    }
    if (!reply->has_value())
    {
        error(ctx, std::format("No response from TCU to {}", label));
        return fail(ErrorKind::Timeout, std::format("no response to {}", label));
    }
    return std::move(**reply);
}

// Retries up to `attempts` times, content-blind: legacy's own condition is
// `if (received != "")` (e.g. lines 142-157, 182-197, 696-711, 770-785,
// 811-826), so ANY reply -- right SID, wrong SID, negative, whatever --
// stops the retry early. A hard transport failure at any attempt is
// immediately fatal (never retried). If every attempt times out, this
// still returns success (an empty optional) so the caller proceeds
// regardless -- legacy's own post-loop code never checks `connected` for
// the TCU ID/CAL ID queries, and its post-loop content check is itself
// commented out for reflash_block's setup/close/checksum siblings.
Result<std::optional<bytes::Bytes>> retry_until_any_reply(Ctx& ctx, bytes::ByteView pdu,
                                                          int attempts, int timeout_ms)
{
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        Result<std::optional<bytes::Bytes>> reply = exchange(ctx, pdu, timeout_ms);
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

// Kernel-alive probe (legacy lines 106-127): 0x31/0x02/0x02/0x01, single
// send+read, no retry. Legacy indexes `received.at(4)` through `.at(7)`
// with NO length guard before this comparison -- unlike every sibling
// family's own alive-probe check. This is also the one exchange in the
// whole function where an ABSENT reply is the routine case (the TCU's
// stock firmware, not yet in the diagnostic kernel, is not expected to
// answer this PDU at all) rather than a sign of a broken connection -- so
// both an empty reply and a present-but-wrong one report "not running";
// only an exact match reports true, and only a genuine transport-level
// failure (Disconnected/Cancelled/malformed envelope) is fatal here.
// uds::payload() bounds-checks the reply bytes past the service id instead
// of raw-indexing off the end of a short buffer, so the port is
// structurally incapable of the legacy UB here, not merely avoiding it by
// convention (see the implementation brief's Step 7).
Result<bool> kernel_already_running(Ctx& ctx)
{
    info(ctx, "Checking if kernel is already running...");
    Result<std::optional<bytes::Bytes>> alive_probe =
        exchange(ctx, bytes::Bytes{0x31, 0x02, 0x02, 0x01}, 200);
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
    const bytes::ByteView rest = uds::payload(reply);
    return rest.size() >= 3 && rest[0] == 0x02 && rest[1] == 0x02 && rest[2] == 0x03;
}

// Shared shape of the TCU ID / CAL ID init retries (legacy lines 129-157 and
// 171-197): retried up to `attempts` times, content-blind, proceeds
// regardless even if every attempt times out. `success_prefix` and
// `no_response_label` carry each site's own legacy log wording verbatim
// (the two success messages are not the same string in legacy either).
Result<std::optional<bytes::Bytes>> retry_init_step(Ctx& ctx, bytes::ByteView pdu, int attempts,
                                                    int timeout_ms, std::string_view success_prefix,
                                                    std::string_view no_response_label)
{
    Result<std::optional<bytes::Bytes>> reply = retry_until_any_reply(ctx, pdu, attempts, timeout_ms);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (reply->has_value())
    {
        info(ctx, std::format("{}{}", success_prefix, bytes::toHex(**reply)));
    }
    else
    {
        info(ctx, std::format("No response to {} after {} attempts -- proceeding regardless",
                              no_response_label, attempts));
    }
    return reply;
}

// Shared shape of every content-blind single-shot exchange below (session
// init, seed, seed key, jump): a mismatch only logs `mismatch_message` --
// legacy's own `// return STATUS_ERROR;` on these checks is commented out --
// and the reply is returned either way for the caller to use.
Result<bytes::Bytes> single_shot_logged(Ctx& ctx, bytes::ByteView pdu, int timeout_ms,
                                        std::string_view label,
                                        std::initializer_list<bytes::Byte> expect,
                                        std::string_view mismatch_message)
{
    Result<bytes::Bytes> reply = single_shot(ctx, pdu, timeout_ms, label);
    if (!reply.has_value())
    {
        return reply;
    }
    if (!matches(*reply, expect))
    {
        error(ctx, mismatch_message);
    }
    return reply;
}

// Alive re-check content test (legacy line 334). Legacy's own condition
// uses `&&` where every sibling exchange in this file uses `||` --
// transcribed literally: true (kernel verified running) only when the
// reply's SID, format id, address-format id, AND length-format id ALL
// simultaneously differ from 0x74/0x20/0x01/0x04, which in practice almost
// never happens for a well-formed reply.
bool kernel_verified_running(const bytes::Bytes& alive_recheck)
{
    return alive_recheck.size() >= 4 && alive_recheck[0] != 0x74 && alive_recheck[1] != 0x20 &&
           alive_recheck[2] != 0x01 && alive_recheck[3] != 0x04;
}

// Legacy connect_bootloader, lines 89-344.
Status connect_bootloader(Ctx& ctx)
{
    Result<bool> running = kernel_already_running(ctx);
    if (!running.has_value())
    {
        return std::unexpected(running.error());
    }
    if (*running)
    {
        info(ctx, "Kernel already running");
        return {};
    }

    info(ctx, "Trying TCU Init...");
    Result<std::optional<bytes::Bytes>> tcu_id =
        retry_init_step(ctx, bytes::Bytes{0xAA}, 6, 200, "Init Success: ", "TCU Init");
    if (!tcu_id.has_value())
    {
        return std::unexpected(tcu_id.error());
    }

    info(ctx, "Trying 0x09 0x04...");
    Result<std::optional<bytes::Bytes>> cal_id = retry_init_step(
        ctx, bytes::Bytes{0x09, 0x04}, 6, 200, "Init Success: TCU ID = ", "0x09 0x04");
    if (!cal_id.has_value())
    {
        return std::unexpected(cal_id.error());
    }

    info(ctx, "Initializing bootloader...");
    Result<bytes::Bytes> session = single_shot_logged(ctx, bytes::Bytes{0x10, 0x43}, 200,
                                                      "session init", {0x50, 0x43},
                                                      "Failed to initialise bootloader");
    if (!session.has_value())
    {
        return std::unexpected(session.error());
    }

    info(ctx, "Starting seed request...");
    Result<bytes::Bytes> seed_reply =
        single_shot_logged(ctx, bytes::Bytes{0x27, 0x01}, 200, "the seed request", {0x67, 0x01},
                           "Bad response to seed request");
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    info(ctx, "Seed request ok");
    // Legacy indexes received.at(6..9) -- the 4 bytes after the
    // subfunction byte of the stripped PDU -- with no length guard (lines
    // 253-256). uds::payload() strips the service id bounds-safely; any
    // seed byte past what the reply actually carries defaults to 0x00
    // rather than reading out of bounds, matching this family's total
    // content tolerance (a short/wrong reply still produces SOME seed key,
    // never a crash).
    bytes::Bytes seed(4, 0x00);
    {
        const bytes::ByteView after_sid = uds::payload(*seed_reply); // [subfunction, seed x4]
        for (std::size_t i = 0; i < 4 && i + 1 < after_sid.size(); ++i)
        {
            seed[i] = after_sid[i + 1];
        }
    }
    const bytes::Bytes key = seed_key(seed);

    // Seed key 0x27/0x02 (lines 260-282): single-shot, content-blind.
    info(ctx, "Sending seed key...");
    bytes::Bytes key_request{0x27, 0x02};
    key_request.insert(key_request.end(), key.begin(), key.end());
    Result<bytes::Bytes> key_reply = single_shot_logged(ctx, key_request, 200, "the seed key",
                                                        {0x67, 0x02}, "Bad response to seed request");
    if (!key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    info(ctx, "Seed key ok");

    info(ctx, "Jumping to onboad kernel...");
    Result<bytes::Bytes> jump_reply =
        single_shot_logged(ctx, bytes::Bytes{0x10, 0x42}, 200, "the kernel jump", {0x50, 0x42},
                           "Bad response to jumping to onboard kernel");
    if (!jump_reply.has_value())
    {
        return std::unexpected(jump_reply.error());
    }
    info(ctx, "Jump to kernel ok");

    // Alive re-check (lines 312-343): sent
    // 0x34/0x04/0x33/0x00/0x00/0x00/0x08/0x00/0x00, single-shot. There is no
    // `else` branch either way -- connect_bootloader falls through to
    // "Test script complete" and returns success (line 343) regardless of
    // kernel_verified_running()'s outcome.
    info(ctx, "Checking if jump successful and kernel alive...");
    Result<bytes::Bytes> alive_recheck = single_shot(
        ctx, bytes::Bytes{0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}, 200,
        "the alive re-check");
    if (!alive_recheck.has_value())
    {
        return std::unexpected(alive_recheck.error());
    }
    if (kernel_verified_running(*alive_recheck))
    {
        info(ctx, "Kernel verified to be running");
    }

    info(ctx, "Test script complete");
    return {};
}

// Legacy read_mem, lines 351-551.
Result<bytes::Bytes> dump_flash_range(Ctx& ctx, PhaseReporter& progress)
{
    constexpr std::uint32_t kPageSize = 0x100;

    // "Settting dump start & length..." (lines 373-401): single-shot,
    // content-blind (line 400's `// return STATUS_ERROR;` is commented
    // out). The trailing 6 bytes are a hardcoded literal, NOT derived from
    // kReadRegion.start/length, matching the sibling MH8111 family's own
    // dump-setup exchange exactly.
    info(ctx, "Settting dump start & length...");
    Result<bytes::Bytes> setup = single_shot(
        ctx, bytes::Bytes{0x35, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}, 200,
        "dump start & length setup");
    if (!setup.has_value())
    {
        return std::unexpected(setup.error());
    }
    if (!matches(*setup, {0x75, 0x20, 0x01, 0x01}))
    {
        error(ctx, "Bad response to setting dump start & length");
    }

    info(ctx, "Start reading ROM, please wait...");
    bytes::Bytes rom;
    rom.reserve(kReadRegion.length);
    for (std::uint32_t addr = kReadRegion.start; addr < kReadRegion.start + kReadRegion.length;
         addr += kPageSize)
    {
        // Legacy stopRequested() at line 423, top of loop.
        if (ctx.cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "read cancelled");
        }

        // 0xB7 dump request (lines 406-454): single-shot per chunk, NO
        // retry, content-blind (line 453's `// return STATUS_ERROR;` for
        // "Page data request failed!" is commented out).
        Result<bytes::Bytes> chunk = single_shot(ctx, composeBe(0xB7_b, u24(addr)), 200,
                                                 std::format("the flash read at 0x{:x}", addr));
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        if (!matches(*chunk, {0xF7}))
        {
            error(ctx, "Page data request failed!");
        }
        const bytes::Bytes decrypted = decrypt_page(uds::payload(*chunk));
        rom.insert(rom.end(), decrypted.begin(), decrypted.end());
        progress.update(static_cast<int>(addr + kPageSize - kReadRegion.start));
    }

    info(ctx, "ROM read complete");

    // "Sending stop command..." (lines 505-532): retried up to 6 times,
    // content-blind, and OUTCOME-blind too -- legacy has no
    // `if (try_count == 6) return ERROR` after this loop at all (unlike
    // erase_mem's/reflash_block's own retry loops, whose post-loop check is
    // merely commented out rather than absent), so read_mem proceeds to
    // decrypt and return success regardless of whether the TCU ever
    // acknowledged stop.
    info(ctx, "Sending stop command...");
    if (Result<std::optional<bytes::Bytes>> stop =
            retry_until_any_reply(ctx, bytes::Bytes{0x37}, 6, 200);
        !stop.has_value())
    {
        return std::unexpected(stop.error());
    }

    return rom;
}

// Legacy erase_mem, lines 847-892: a single send, NOT a retry loop (unlike
// the sibling MH8111 family's own erase, which this port must not confuse
// this one with -- see the implementation brief's warning on this exact
// point).
Status erase_memory(Ctx& ctx)
{
    info(ctx, "Erasing TCU ROM...");
    if (const Status sent = ctx.channel.send(
            bytes::Bytes{0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff}, ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    // Legacy delay(8000) before reading the erase acknowledgement (line
    // 877) -- the TCU needs this long to actually erase flash before it
    // can answer. Modeled via ctx.clock.sleep() so tests assert the timing
    // parameter instead of literally waiting 8 seconds.
    if (const Status slept = ctx.clock.sleep(8000, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }
    Result<std::optional<bytes::Bytes>> reply = ctx.channel.receive(200, ctx.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    // Legacy delay(5000) unconditionally after reading (line 883).
    if (const Status slept = ctx.clock.sleep(5000, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }
    if (!reply->has_value())
    {
        error(ctx, "No response from TCU to the erase command");
        return fail(ErrorKind::Timeout, "no response to the erase command");
    }
    // Content check (lines 885-889) is non-fatal -- `// return
    // STATUS_ERROR;` is commented out.
    if (!matches(**reply, {0x71, 0x01, 0x02}))
    {
        error(ctx, "Erasing error! Do not panic, do not reset the TCU immediately. The kernel "
                   "is most likely still running and receiving commands!");
    }
    return {};
}

// Legacy reflash_block, lines 643-840, called once (this family flashes
// exactly one block, kWriteRegion / fblocks_MH8104[3]).
Status unlock_and_reflash_block(Ctx& ctx, bytes::ByteView block_plain, PhaseReporter& progress)
{
    constexpr std::uint32_t kChunkSize = 128;

    // "Settting flash start & length..." (lines 677-716): retried up to 6
    // times, content-blind for retry purposes (`if (received != "")`) and
    // non-fatal even after exhausting every attempt (line 716's `// return
    // STATUS_ERROR;` is commented out). Unlike MH8111's own setup packet
    // (whose declared data_len is halved by a legacy arithmetic bug --
    // maxblocks computed with a *128 unit but the write loop chunks at
    // *256), MH8104's maxblocks and data_len both use the SAME 128-byte
    // unit (`maxblocks = pl_len / 128; data_len = maxblocks * 128`), and
    // pl_len (0x78000) divides evenly by 128, so data_len == pl_len ==
    // kWriteRegion.length exactly -- confirmed directly against the
    // source, not assumed from the sibling family's own halving quirk.
    info(ctx, "Settting flash start & length...");
    Result<std::optional<bytes::Bytes>> setup = retry_until_any_reply(
        ctx,
        composeBe(0x34_b, 0x04_b, 0x33_b, u24(kWriteRegion.start), u24(kWriteRegion.length)), 6,
        200);
    if (!setup.has_value())
    {
        return std::unexpected(setup.error());
    }
    if (!setup->has_value() || !matches(**setup, {0x74}))
    {
        error(ctx, "No or bad response received");
    }

    const bytes::Bytes encrypted = encrypt_rom(block_plain);

    for (std::uint32_t offset = 0; offset < kWriteRegion.length; offset += kChunkSize)
    {
        // Legacy's stopRequested() check (line 721) returns 0 (success) to
        // the caller on cancellation -- the same bug Tasks 1 and 3 already
        // disclosed and diverged from. Not reproduced here: a cancellation
        // mid-write is reported as Cancelled, never success.
        if (ctx.cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "write cancelled");
        }

        const std::uint32_t addr = kWriteRegion.start + offset;
        const bytes::ByteView chunk_data = bytes::ByteView(encrypted).subspan(offset, kChunkSize);
        // Legacy reads but never inspects the per-chunk reply at all (lines
        // 746-751, receive_timeout=500) -- not even for presence: only a
        // transport-level failure from the read itself fails this
        // exchange, unlike every single-shot exchange above.
        if (const Status sent =
                ctx.channel.send(composeBe(0xB6_b, u24(addr), chunk_data), ctx.cancellation);
            !sent.has_value())
        {
            return sent;
        }
        if (Result<std::optional<bytes::Bytes>> reply = ctx.channel.receive(500, ctx.cancellation);
            !reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        progress.update(static_cast<int>(offset + kChunkSize));
    }

    // Closing (lines 758-791): retried up to 6 times, content-blind,
    // non-fatal even after exhausting every attempt (line 791's `// return
    // STATUS_ERROR;` is commented out).
    info(ctx, "Closing out Flashing of this block...");
    Result<std::optional<bytes::Bytes>> closed =
        retry_until_any_reply(ctx, bytes::Bytes{0x37}, 6, 200);
    if (!closed.has_value())
    {
        return std::unexpected(closed.error());
    }
    if (!closed->has_value() || !matches(**closed, {0x77}))
    {
        error(ctx, "No or bad response received");
    }

    // Legacy delay(100) between close and checksum (line 793).
    if (const Status slept = ctx.clock.sleep(100, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    // "Verifying checksum..." (lines 795-832): retried up to 6 times,
    // content-blind, non-fatal even after exhausting every attempt (line
    // 832's `// return STATUS_ERROR;` is commented out).
    info(ctx, "Verifying checksum...");
    Result<std::optional<bytes::Bytes>> checksum =
        retry_until_any_reply(ctx, bytes::Bytes{0x31, 0x01, 0x02, 0x02, 0x01}, 6, 200);
    if (!checksum.has_value())
    {
        return std::unexpected(checksum.error());
    }
    if (!checksum->has_value() || !matches(**checksum, {0x71, 0x01, 0x02}))
    {
        error(ctx, "No or bad response received");
    }
    else
    {
        info(ctx, "Checksum verified...");
    }

    return {};
}

// Legacy write_mem, lines 559-636: erase_mem() first, then exactly one
// reflash_block() call for the sole modified block (index 3).
Status write_mem(Ctx& ctx, bytes::ByteView image, PhaseSequence& phases)
{
    PhaseReporter erase = phases.start("Erase", 1);
    if (const Status erased = erase_memory(ctx); !erased.has_value())
    {
        return erased;
    }
    erase.complete();

    PhaseReporter write = phases.start("Write ROM", static_cast<int>(kWriteRegion.length));
    const bytes::ByteView block_plain = image.subspan(kWriteRegion.start, kWriteRegion.length);
    if (const Status written = unlock_and_reflash_block(ctx, block_plain, write);
        !written.has_value())
    {
        return written;
    }
    write.complete();
    return {};
}

} // namespace

Result<FlashExecutionResult> SubaruTcuCvtMitsuMh8104CanExecutor::execute(
    const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events)
{
    if (const Status matched = check_family_transport_match(
            plan, FlashFamily::SubaruTcuCvtMitsuMh8104Can, TransportKind::CanIso15765);
        !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = validate_subaru_tcu_cvt_mitsu_mh8104_can_plan(plan);
        !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruTcuCvtMitsuMh8104CanPlan>(plan.family_plan());
    auto *can = dynamic_cast<ICanFlashTransport *>(&transport);
    if (can == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "transport does not implement ICanFlashTransport");
    }

    const bool read = plan.operation() == FlashOperation::Read;
    PhaseSequence phases(events, read ? 2 : 3);
    PhaseReporter connect = phases.start(read ? "Connect to TCU" : "Connect", 1);

    if (const Status configured = can->configure(Iso15765Config{
            .bitrate = family.bitrate,
            .request_id = family.request_id,
            .response_id = family.response_id,
            .extended_id = family.extended_id,
        });
        !configured.has_value())
    {
        return std::unexpected(configured.error());
    }
    if (const Status opened = can->open(); !opened.has_value())
    {
        return std::unexpected(opened.error());
    }

    CanFlashUdsChannel channel(*can, family.request_id, family.response_id);
    Ctx ctx{cancellation, events, clock, channel};

    info(ctx, "Connecting to Subaru TCU Mitsubishi CAN bootloader, please wait...");
    if (const Status connected = connect_bootloader(ctx); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connect.complete();

    if (plan.operation() == FlashOperation::Read)
    {
        events.notice("Reading ROM, please wait...");
        info(ctx, "Reading ROM from TCU Subaru Mitsubishi using CAN");

        PhaseReporter read_phase =
            phases.start("Read ROM", static_cast<int>(plan.transfer_region().length));
        Result<bytes::Bytes> window = dump_flash_range(ctx, read_phase);
        if (!window.has_value())
        {
            return std::unexpected(window.error());
        }
        read_phase.complete();

        // Legacy pads the unread low 0x8000 region with 0xFF (lines
        // 537-544), the same padding shape as the sibling MH8111 family.
        bytes::Bytes rom(kReadRegion.start, 0xFF);
        rom.insert(rom.end(), window->begin(), window->end());
        return FlashExecutionResult{
            .operation = FlashOperation::Read,
            .read_bytes = std::move(rom),
        };
    }

    // build_subaru_tcu_cvt_mitsu_mh8104_can_plan refuses TestWrite; the
    // guard is repeated here so a plan built another way cannot turn a dry
    // run into a real erase and write.
    if (plan.operation() != FlashOperation::Write)
    {
        return fail(ErrorKind::Unsupported,
                    "test_write is not supported by the Subaru TCU CVT Mitsu MH8104 CAN family");
    }

    events.notice("Writing ROM, please wait...");
    info(ctx, "Writing ROM to Subaru Mitsubishi CAN 32bit TCUs, on board kernel");
    if (const Status written = write_mem(ctx, *plan.image(), phases); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{
        .operation = plan.operation(),
        .read_bytes = std::nullopt,
    };
}

} // namespace fastecu::flash
