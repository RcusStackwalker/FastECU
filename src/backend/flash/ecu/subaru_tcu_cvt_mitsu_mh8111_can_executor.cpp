#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.h"

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
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_plan.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can_operation.cpp
// it was transcribed from. Unlike SubaruTcuCvtHitachiM32rCanExecutor (Task
// 3), every one of this family's exchanges (identity queries, session,
// seed, jump, alive check) is sent on this family's OWN 0x7e1 envelope --
// there is no second "other id" CanFlashUdsChannel/UdsClient pair here.
namespace fastecu::flash
{
namespace
{
using namespace bytes::literals;
using bytes::composeBe;
using bytes::u24;

constexpr uds::ExchangePolicy kExchangePolicy{.read_timeout_ms = 2000};

// Seed key (legacy generate_seed_key, lines 908-914).
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{
    0x9E99, 0x685C, 0x874D, 0xF11E, 0x27D4, 0xA967, 0xB63B, 0x7A37,
    0xE23B, 0xA8D0, 0x9B82, 0xAC43, 0xE874, 0x7FC5, 0x7141, 0x8B44};
// Encrypt (write payload, legacy encrypt_payload lines 947-948).
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x7bf2, 0xa8b4, 0x4492, 0x6587};
// Decrypt (read payload, legacy decrypt_payload lines 965-966).
constexpr std::array<std::uint16_t, 4> kDecryptTable{0x6587, 0x4492, 0xa8b4, 0x7bf2};
// Shared by every family in this wave and wave-1 Hitachi K-Line.
constexpr std::array<std::uint8_t, 32> kIndexTransformation{
    0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
    0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

// Hardcoded in legacy read_mem regardless of caller arguments (lines
// 356-358, "hack for testing").
constexpr MemoryRegion kReadRegion{0x8000, 0x78000};
// The sole flashed block (block_modified skips 0-2, flashes only block 3 --
// fblocks_MH8111[3] in kernelmemorymodels.h). Deliberately does NOT overlap
// kReadRegion -- see subaru_tcu_cvt_mitsu_mh8111_can_plan.cpp's comment.
constexpr MemoryRegion kWriteRegion{0x80000, 0x100000};

bytes::Bytes seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable.data(), kIndexTransformation.data());
}

bytes::Bytes encrypt_rom(bytes::ByteView image)
{
    return SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()),
                                         kEncryptTable.data(), kIndexTransformation.data());
}

bytes::Bytes decrypt_page(bytes::ByteView page)
{
    return SsmProtocol::calculatePayload(page, static_cast<std::uint32_t>(page.size()),
                                         kDecryptTable.data(), kIndexTransformation.data());
}

struct Ctx
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    IClock& clock;
    uds::UdsClient& uds;
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

// Mirrors every other wave-3 executor's report_exchange_failure.
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

// Sends `pdu` through ctx.uds and, on failure, logs and returns the error.
// Used for every exchange whose expected reply follows the standard
// SID+0x40 positive-response convention that UdsClient itself enforces.
Result<bytes::Bytes> fatal_request(Ctx& ctx, bytes::ByteView pdu, std::string_view operation)
{
    Result<bytes::Bytes> reply = ctx.uds.request(pdu, kExchangePolicy, ctx.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(
            report_exchange_failure(ctx, reply.error(), "Wrong response from TCU: ", operation));
    }
    return reply;
}

// Legacy's two non-fatal identity queries (TCU ID/CAL ID, lines 94-168) and
// the non-fatal session request (lines 173-197): logged on mismatch or
// absence but never halt connect_bootloader -- even a genuine exchange
// failure is logged and swallowed, mirroring legacy's total absence of an
// early return in this block.
void non_fatal_query(Ctx& ctx, bytes::ByteView pdu, std::optional<bytes::Byte> expected_subfunction,
                     std::string_view label)
{
    Result<bytes::Bytes> reply = ctx.uds.request(pdu, kExchangePolicy, ctx.cancellation);
    if (!reply.has_value())
    {
        error(ctx, std::format("Wrong response from TCU: {}", reply.error().detail));
        return;
    }
    const bytes::ByteView payload = uds::payload(*reply);
    if (expected_subfunction.has_value() &&
        (payload.empty() || payload[0] != *expected_subfunction))
    {
        error(ctx, "Wrong response from TCU: unexpected subfunction");
        return;
    }
    info(ctx, std::format("{}: {}", label, bytes::toHex(payload)));
}

// Legacy connect_bootloader, lines 81-337. Unlike SubaruTcuCvtHitachiM32rCan
// (Task 3), this family has NO kernel-alive pre-check shortcut -- the full
// 7-exchange sequence below runs every time (Test 2,
// ConnectFullSequenceEveryTime, pins this).
Status connect_bootloader(Ctx& ctx)
{
    // TCU ID 0xAA (lines 94-133), non-fatal.
    info(ctx, "Requesting TCU ID");
    non_fatal_query(ctx, bytes::Bytes{0xAA}, std::nullopt, "TCU ID");

    // CAL ID 0x09/0x04 (lines 135-168), non-fatal.
    info(ctx, "Requesting CAL ID...");
    non_fatal_query(ctx, bytes::Bytes{0x09, 0x04}, bytes::Byte(0x04), "CAL ID");

    // Session 0x10/0x43 (lines 170-197), non-fatal -- the match branch (line
    // 186-188) is empty in legacy; only the mismatch branch logs.
    info(ctx, "Initializing bootloader...");
    non_fatal_query(ctx, bytes::Bytes{0x10, 0x43}, bytes::Byte(0x43), "session mode (bootloader)");

    // Seed 0x27/0x01 (lines 199-225), fatal.
    info(ctx, "Requesting seed");
    Result<bytes::Bytes> seed_reply = fatal_request(ctx, bytes::Bytes{0x27, 0x01}, "the seed request");
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    if (seed_reply->size() < 6 || (*seed_reply)[0] != 0x67 || (*seed_reply)[1] != 0x01)
    {
        error(ctx, std::format("Wrong response from TCU: {}", bytes::toHex(*seed_reply)));
        return fail(ErrorKind::BadResponse, "seed request rejected");
    }
    info(ctx, "Seed request ok");
    const bytes::ByteView seed = bytes::ByteView(*seed_reply).subspan(2, 4);
    const bytes::Bytes key = seed_key(seed);

    // Seed key 0x27/0x02 (lines 236-263), fatal.
    info(ctx, "Sending seed key");
    bytes::Bytes key_request{0x27, 0x02};
    key_request.insert(key_request.end(), key.begin(), key.end());
    Result<bytes::Bytes> key_reply = fatal_request(ctx, key_request, "the seed key");
    if (!key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    if (key_reply->size() < 2 || (*key_reply)[0] != 0x67 || (*key_reply)[1] != 0x02)
    {
        error(ctx, std::format("Wrong response from TCU: {}", bytes::toHex(*key_reply)));
        return fail(ErrorKind::BadResponse, "seed key rejected");
    }
    info(ctx, "Seed key ok");

    // Jump 0x10/0x42 (lines 267-295), fatal.
    info(ctx, "Jumping to onboad kernel...");
    Result<bytes::Bytes> jump_reply = fatal_request(ctx, bytes::Bytes{0x10, 0x42}, "the kernel jump");
    if (!jump_reply.has_value())
    {
        return std::unexpected(jump_reply.error());
    }
    if (uds::subfunction(*jump_reply) != 0x42)
    {
        error(ctx, "Wrong response from TCU: unexpected jump response");
        return fail(ErrorKind::BadResponse, "kernel jump rejected");
    }
    info(ctx, "Jump to kernel ok");

    // Alive check (lines 298-331). The SENT PDU is
    // 0x34/0x04/0x33/0x00/0x00/0x00/0x08/0x00/0x00 -- the SAME literal
    // trailing bytes as dump_flash_range's dump-setup exchange and (by
    // coincidence of this block's own halved data_len) unlock_and_reflash_
    // block's setup exchange -- but the checked reply is
    // 0x71/0x02/0x02/0x03, the SID+0x40 form of a DIFFERENT service (0x31),
    // not 0x74 (0x34's own SID+0x40). Confirmed directly against the
    // source, re-read twice; the brief's Step 7 description of this step's
    // sent bytes as 0x31/0x02/0x02/0x01 (Task 3's own alive re-check PDU)
    // does not match the actual legacy source and is disclosed here as a
    // found brief/source discrepancy, not silently resolved either way.
    // Because the reply's service byte does not match the sent PDU's own
    // SID, this cannot go through fatal_request()/ctx.uds (UdsClient::
    // request() would reject any 0x71-led reply as "expected response to
    // SID 0x34, got 0x71", making this step always fail) -- routed through
    // the channel directly instead.
    info(ctx, "Checking if jump successful and kernel alive...");
    if (const Status sent = ctx.channel.send(
            bytes::Bytes{0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}, ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> alive = ctx.channel.receive(2000, ctx.cancellation);
    if (!alive.has_value())
    {
        return std::unexpected(alive.error());
    }
    if (!alive->has_value() || alive->value().size() < 4 || (**alive)[0] != 0x71 ||
        (**alive)[1] != 0x02 || (**alive)[2] != 0x02 || (**alive)[3] != 0x03)
    {
        if (alive->has_value())
        {
            error(ctx, std::format("Wrong response from TCU: {}", bytes::toHex(**alive)));
        }
        else
        {
            error(ctx, "No valid response from ECU");
        }
        return fail(ErrorKind::BadResponse, "kernel alive check failed");
    }

    info(ctx, "Kernel verified to be running");
    return {};
}

// Legacy read_mem, lines 344-544.
Result<bytes::Bytes> dump_flash_range(Ctx& ctx, PhaseReporter& progress)
{
    constexpr std::uint32_t kPageSize = 0x100;

    // "Settting dump start & length..." (lines 365-400). The sent PDU's SID
    // is 0x35, but the checked ack is 0x74 -- the same "set up transfer"
    // ack byte unlock_and_reflash_block's own (0x34-prefixed) setup
    // exchange checks, not the standard SID+0x40 response to 0x35 (which
    // would be 0x75). The trailing 6 bytes are a hardcoded literal, NOT
    // derived from kReadRegion.start/length (0x8000/0x78000) -- confirmed
    // directly against the source. Because the ack's service byte does not
    // match the sent PDU's own SID, this cannot go through fatal_request()/
    // ctx.uds either -- routed through the channel directly, mirroring the
    // alive-check exchange's own necessity in connect_bootloader.
    info(ctx, "Settting dump start & length...");
    if (const Status sent = ctx.channel.send(
            bytes::Bytes{0x35, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}, ctx.cancellation);
        !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    Result<std::optional<bytes::Bytes>> setup = ctx.channel.receive(2000, ctx.cancellation);
    if (!setup.has_value())
    {
        return std::unexpected(setup.error());
    }
    if (!setup->has_value() || setup->value().size() < 4 || (**setup)[0] != 0x74 ||
        (**setup)[1] != 0x20 || (**setup)[2] != 0x01 || (**setup)[3] != 0x04)
    {
        if (setup->has_value())
        {
            error(ctx, std::format("Wrong response from TCU: {}", bytes::toHex(**setup)));
        }
        else
        {
            error(ctx, "No valid response from ECU");
        }
        return fail(ErrorKind::BadResponse, "dump start & length setup rejected");
    }

    info(ctx, "Start reading ROM, please wait...");
    bytes::Bytes rom;
    rom.reserve(kReadRegion.length);
    for (std::uint32_t addr = kReadRegion.start; addr < kReadRegion.start + kReadRegion.length;
         addr += kPageSize)
    {
        // Legacy stopRequested() at line 421, top of loop.
        if (ctx.cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "read cancelled");
        }

        // Lines 406-454: SID 0xB7 + 3-byte big-endian address, standard
        // SID+0x40 convention (0xB7 -> 0xF7).
        Result<bytes::Bytes> chunk = fatal_request(ctx, composeBe(0xB7_b, u24(addr)),
                                                   std::format("the flash read at 0x{:x}", addr));
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        const bytes::Bytes decrypted = decrypt_page(uds::payload(*chunk));
        rom.insert(rom.end(), decrypted.begin(), decrypted.end());
        progress.update(static_cast<int>(addr + kPageSize - kReadRegion.start));
    }

    info(ctx, "ROM read complete");

    // "Sending stop command..." (lines 504-526): resent up to 6 times,
    // content-blind (legacy checks only `received != ""`, never an SID or
    // content match) -- and OUTCOME-blind too: legacy has no
    // `if (try_count == 6) return ERROR` after this loop at all (unlike
    // erase_mem's/unlock_and_reflash_block's own retry loops below, which
    // do gate on the final try_count), so read_mem proceeds to decrypt and
    // return success regardless of whether the TCU ever acknowledged stop.
    // Routed through the channel directly, both because content is never
    // checked and because a "failure" here must never fail the read.
    info(ctx, "Sending stop command...");
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        if (const Status sent = ctx.channel.send(bytes::Bytes{0x37}, ctx.cancellation);
            !sent.has_value())
        {
            return std::unexpected(sent.error());
        }
        Result<std::optional<bytes::Bytes>> reply = ctx.channel.receive(800, ctx.cancellation);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        if (reply->has_value())
        {
            break;
        }
    }

    return rom;
}

// Legacy erase_mem, lines 833-892. The literal legacy loop never resends
// (it writes the erase command once, above the loop, then only re-READS)
// and its mismatch branch's `return STATUS_ERROR` is commented out with no
// success branch anywhere -- so `try_count` always reaches 20 and the
// function ALWAYS returns STATUS_ERROR, regardless of what the TCU
// answers. That would make write_mem's very first step
// (`if (erase_mem()) { ...; return STATUS_ERROR; }`) fail unconditionally,
// so every write in this family has never worked in production. The
// brief's own description of this loop ("retried ... until <response> ...
// fatal after 20 failed attempts") presupposes a working retry-until-match
// shape, matching the sibling close/checksum retry loops in this very
// file (both of which DO resend each attempt and DO break/return on an
// exact match). This targets that evident intent instead -- disclosed here
// as this family's one deliberate divergence from the literal legacy
// source, alongside Tasks 1 and 3's own disclosed findings.
Status erase_memory(Ctx& ctx)
{
    info(ctx, "Erasing TCU ROM...");
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        Result<bytes::Bytes> reply = ctx.uds.request(
            bytes::Bytes{0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff}, kExchangePolicy,
            ctx.cancellation);
        if (reply.has_value())
        {
            const bytes::ByteView payload = uds::payload(*reply);
            if (payload.size() >= 2 && payload[0] == 0x01 && payload[1] == 0x02)
            {
                info(ctx, "Erased! Starting Writing! Do Not Power Off!");
                return {};
            }
            error(ctx, "Wrong response from TCU: unexpected erase acknowledgement");
        }
        else if (reply.error().kind == ErrorKind::Cancelled)
        {
            return std::unexpected(report_exchange_failure(
                ctx, reply.error(), "Wrong response from TCU: ", "the erase command"));
        }
        else
        {
            error(ctx, std::format("Wrong response from TCU: {}", reply.error().detail));
        }
    }
    error(ctx, "Flash area erase failed");
    return fail(ErrorKind::BadResponse, "flash area erase failed");
}

// Legacy reflash_block, lines 636-826, called once (this family flashes
// exactly one block, kWriteRegion / fblocks_MH8111[3]).
Status unlock_and_reflash_block(Ctx& ctx, bytes::ByteView block_plain, PhaseReporter& progress)
{
    constexpr std::uint32_t kChunkSize = 256;

    // maxblocks/end_addr/data_len (lines 661-663): maxblocks = pl_len/256
    // (the real chunk count the loop below uses -- 4096 for this block's
    // 0x100000 bytes), but end_addr is computed as start + maxblocks*128
    // (HALF a chunk's width per block, not *256), so data_len == pl_len/2
    // == 0x80000 for this block. This is a genuine legacy arithmetic bug in
    // the value DECLARED in the setup packet below -- distinct from
    // erase_mem's control-flow bug above, this one does not prevent the
    // write from working (the chunk loop below still sends the full
    // 0x100000 bytes; only the setup packet's own length field is wrong)
    // -- so it is preserved exactly rather than corrected, per this wave's
    // established precedent for found-but-harmless byte-value quirks
    // (Task 3's block-size finding).
    constexpr std::uint32_t kMaxBlocks = kWriteRegion.length / kChunkSize; // 4096
    constexpr std::uint32_t kSetupDataLen = kMaxBlocks * 128;              // 0x80000

    // "Settting flash start & length..." (lines 670-705): retried up to 6
    // times, fatal after 6. Legacy DOES check content here
    // (`received.at(4) == 0x74`, confirmed directly against the source --
    // the brief's Step 7 description of this step as content-blind does
    // not match), but that check is exactly the standard SID+0x40 match
    // ctx.uds.request() already enforces (0x34 -> 0x74), so no extra check
    // is added here beyond the retry-count wrapper.
    info(ctx, "Settting flash start & length...");
    bool setup_ok = false;
    for (int attempt = 0; attempt < 6 && !setup_ok; ++attempt)
    {
        Result<bytes::Bytes> setup = ctx.uds.request(
            composeBe(0x34_b, 0x04_b, 0x33_b, u24(0), u24(kSetupDataLen)), kExchangePolicy,
            ctx.cancellation);
        setup_ok = setup.has_value();
    }
    if (!setup_ok)
    {
        return fail(ErrorKind::BadResponse, "flash start & length setup rejected");
    }

    const bytes::Bytes encrypted = encrypt_rom(block_plain);

    for (std::uint32_t offset = 0; offset < kWriteRegion.length; offset += kChunkSize)
    {
        // Legacy's stopRequested() check (line 710) returns 0 -- success --
        // to the caller on cancellation; the same bug Tasks 1 and 3 already
        // disclosed and diverged from. Not reproduced here either: a
        // cancellation mid-write is reported as Cancelled, never success.
        if (ctx.cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "write cancelled");
        }

        const std::uint32_t addr = kWriteRegion.start + offset;
        const bytes::ByteView chunk_data = bytes::ByteView(encrypted).subspan(offset, kChunkSize);
        // Legacy reads but never inspects the per-chunk reply at all (lines
        // 734-736) -- only a transport-level failure from the read itself
        // fails this exchange. Routed through the channel directly, not
        // fatal_request()/ctx.uds, which would enforce a SID+0x40 content
        // match this family's legacy code never checks here.
        if (const Status sent =
                ctx.channel.send(composeBe(0xB6_b, u24(addr), chunk_data), ctx.cancellation);
            !sent.has_value())
        {
            return sent;
        }
        Result<std::optional<bytes::Bytes>> reply = ctx.channel.receive(2000, ctx.cancellation);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        progress.update(static_cast<int>(offset + kChunkSize));
    }

    // Closing (lines 744-782): retried up to 20 times, fatal after 20 --
    // unlike Task 1's ECU family (6-attempt, tolerant-of-anything-including-
    // failure close) and Task 3's own close (single fatal attempt, no
    // retry). Content beyond the standard SID+0x40 match (0x37 -> 0x77) is
    // never checked, matching ctx.uds.request()'s own enforcement exactly.
    info(ctx, "Closing out Flashing of this block...");
    bool closed_ok = false;
    for (int attempt = 0; attempt < 20 && !closed_ok; ++attempt)
    {
        Result<bytes::Bytes> closed =
            ctx.uds.request(bytes::Bytes{0x37}, kExchangePolicy, ctx.cancellation);
        if (closed.has_value())
        {
            info(ctx, "Flashing of block closed");
            closed_ok = true;
        }
    }
    if (!closed_ok)
    {
        return fail(ErrorKind::BadResponse, "block close failed");
    }

    // "Verifying checksum..." (lines 786-825): retried up to 20 times,
    // fatal after 20 -- the same shape as erase_mem's own (fixed) retry
    // loop above.
    info(ctx, "Verifying checksum...");
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        Result<bytes::Bytes> checksum = ctx.uds.request(
            bytes::Bytes{0x31, 0x01, 0x02, 0x02, 0x01}, kExchangePolicy, ctx.cancellation);
        if (checksum.has_value())
        {
            const bytes::ByteView payload = uds::payload(*checksum);
            if (payload.size() >= 2 && payload[0] == 0x01 && payload[1] == 0x02)
            {
                info(ctx, "Checksum verified...");
                return {};
            }
            error(ctx, "Wrong response from TCU: ROM checksum error");
        }
        else
        {
            error(ctx, std::format("Wrong response from TCU: {}", checksum.error().detail));
        }
    }
    return fail(ErrorKind::BadResponse, "ROM checksum verify failed");
}

// Legacy write_mem, lines 552-629: erase_mem() first, then exactly one
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

Result<FlashExecutionResult> SubaruTcuCvtMitsuMh8111CanExecutor::execute(
    const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events)
{
    if (const Status matched = check_family_transport_match(
            plan, FlashFamily::SubaruTcuCvtMitsuMh8111Can, TransportKind::CanIso15765);
        !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = validate_subaru_tcu_cvt_mitsu_mh8111_can_plan(plan);
        !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruTcuCvtMitsuMh8111CanPlan>(plan.family_plan());
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
    uds::UdsClient uds_client(channel, clock, events);
    Ctx ctx{cancellation, events, clock, uds_client, channel};

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
        // 531-538 -- the comment above the loop says "pad ... with 0x00"
        // but the loop body writes 0xFF; the code, not the misleading
        // comment, is preserved). This differs from
        // SubaruTcuCvtHitachiM32rCan (Task 3), which pads with 0x00.
        bytes::Bytes rom(kReadRegion.start, 0xFF);
        rom.insert(rom.end(), window->begin(), window->end());
        return FlashExecutionResult{
            .operation = FlashOperation::Read,
            .read_bytes = std::move(rom),
        };
    }

    // build_subaru_tcu_cvt_mitsu_mh8111_can_plan refuses TestWrite; the
    // guard is repeated here so a plan built another way cannot turn a dry
    // run into a real erase and write.
    if (plan.operation() != FlashOperation::Write)
    {
        return fail(ErrorKind::Unsupported,
                    "test_write is not supported by the Subaru TCU CVT Mitsu MH8111 CAN family");
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
