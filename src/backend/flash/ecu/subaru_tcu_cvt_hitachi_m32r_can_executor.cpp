#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.h"

#include <array>
#include <cstdint>
#include <format>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can_operation.cpp
// it was transcribed from. This is the family whose execute() calls the
// always-failing hack_words() instead of connect_bootloader()/read_mem()/
// write_mem() -- those three (plus their private helpers) are the real,
// previously-unreachable logic this file ports; hack_words() itself (lines
// 415-492) is not ported. See the wave-3 design's "Deliberate divergence"
// section and subaru_tcu_cvt_hitachi_m32r_can_types.h.
namespace fastecu::flash
{
namespace
{
using namespace bytes::literals;
using bytes::composeBe;
using bytes::u24;

// Most exchanges in legacy read serial_read_timeout (2000ms); a handful
// (the two alive probes, the kernel jump, and the dump-setup call) use an
// explicit 200ms window instead. Every exchange in this executor -- both
// UdsClient pairs alike -- uses one flat 2000ms policy, matching
// subaru_hitachi_m32r_can_executor.cpp's own precedent of a single uniform
// ExchangePolicy rather than reproducing legacy's per-step timing variance,
// which has no bearing on wire-byte correctness.
constexpr uds::ExchangePolicy kExchangePolicy{.read_timeout_ms = 2000};

// Session id in ISO 14229-1's 0x40-0x5F vehicle-manufacturer-specific band.
// Unlike this family's own session request (0x10/0x03, standard
// extendedDiagnosticSession) and its kernel jump (0x10/0x02, standard
// programmingSession), this second session request has no standard meaning.
constexpr bytes::Byte kSessionBootload = 0x43;

// Seed key.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{0x9E99, 0x685C, 0x874D, 0xF11E, 0x27D4, 0xA967, 0xB63B, 0x7A37,
                                                      0xE23B, 0xA8D0, 0x9B82, 0xAC43, 0xE874, 0x7FC5, 0x7141, 0x8B44};
// Encrypt (write payload).
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x3B61, 0x8BEF, 0x9E51, 0x1075};
// Decrypt (read payload) -- reverse order of kEncryptTable, same values.
constexpr std::array<std::uint16_t, 4> kDecryptTable{0x1075, 0x9E51, 0x8BEF, 0x3B61};
// Shared by all four wave-3 families and wave-1 Hitachi K-Line.
constexpr std::array<std::uint8_t, 32> kIndexTransformation{0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                            0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                            0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

// The resolved read/write window (see the plan's kReadRegion/kWriteRegion
// comment): legacy's own start_addr - 0x00100000 bias underflows for the
// only start_addr ever passed (0) and bypasses its own floor clamp; this
// targets the clamp's evident intent instead.
constexpr MemoryRegion kWindow{0x8000, 0x78000};

// The "other" envelope six of connect_bootloader's exchanges are sent on
// (see the comment above connect_bootloader's session/seed block). The
// response side of that envelope isn't independently specified by legacy
// (which never validates an incoming envelope id at all): the physical CAN
// addressing is fixed once per session by can->configure() below to
// (family.request_id, family.response_id) and never varies per exchange, so
// every reply -- on either envelope -- arrives framed with family.response_id.
constexpr std::uint32_t kOtherRequestId = 0x7e0;

// M32R_512KB's 11 flash blocks (src/backend/definitions/kernelmemorymodels.h,
// fblocks_M32R_512KB): block 3 is 0x8000 bytes, NOT the uniform 0x10000 the
// wave-3 plan's Global Constraints table states for it (a transcription slip
// there -- confirmed directly against the source, block 3 is
// {0x00008000, 0x00008000}, not {0x00008000, 0x00010000}). Legacy's
// block_modified mask skips blocks 0-2 and flashes 3-10 (8 blocks); those 8
// blocks' lengths sum to exactly kWindow.length (0x78000), but are NOT eight
// uniform 64KiB blocks: one 32KiB block (index 3) followed by seven 64KiB
// blocks (indices 4-10). write_mem calls reflash_block once per block, with
// that block's own fdt->fblocks[blockno].start/.len (lines 750-796).
constexpr std::array<MemoryRegion, 8> kWriteBlocks{{
    {0x08000, 0x08000},
    {0x10000, 0x10000},
    {0x20000, 0x10000},
    {0x30000, 0x10000},
    {0x40000, 0x10000},
    {0x50000, 0x10000},
    {0x60000, 0x10000},
    {0x70000, 0x10000},
}};

bytes::Bytes seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable, kIndexTransformation);
}

bytes::Bytes encrypt_rom(bytes::ByteView image)
{
    return SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()), kEncryptTable,
                                         kIndexTransformation);
}

bytes::Bytes decrypt_page(bytes::ByteView page)
{
    return SsmProtocol::calculatePayload(page, static_cast<std::uint32_t>(page.size()), kDecryptTable,
                                         kIndexTransformation);
}

// `channel`/`uds` are bound to this family's own 0x7e1/0x7e9 pair.
// `other_uds` wraps a second CanFlashUdsChannel over the SAME underlying
// ICanFlashTransport, bound to (0x7e0, family.response_id) -- six of
// connect_bootloader's exchanges are sent on that "other" envelope; see the
// comment above connect_bootloader's session/seed block. Both UdsClients are
// used strictly sequentially (never concurrently -- this executor issues one
// exchange at a time), and neither CanFlashUdsChannel nor UdsClient holds any
// state beyond their constructor arguments, so two live instances over one
// transport are safe.
struct Ctx
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    IClock& clock;
    uds::UdsClient& uds;
    uds::UdsClient& other_uds;
    uds::IUdsChannel& channel;
};

void info(Ctx& ctx, std::string_view message)
{
    ctx.events.log(LogLevel::Info, message);
}

void error(Ctx& ctx, std::string_view message)
{
    ctx.events.log(LogLevel::Error, message);
}

constexpr std::string_view kRejectionPrefix = "Wrong response from TCU: ";

// Thin wrapper over the shared report_exchange_failure
// (uds_client_exchange_common.h) for the two exchanges below that read a
// raw uds::IUdsChannel reply directly instead of going through
// fatal_request -- their expected reply does not follow the standard
// SID+0x40 convention UdsClient itself enforces.
Error report_exchange_failure(Ctx& ctx, const Error& failure, std::string_view rejection_prefix,
                              std::string_view operation)
{
    return ::fastecu::flash::report_exchange_failure(ctx.events, failure, rejection_prefix, operation);
}

// Sends `pdu` through `client` and, on failure, logs and returns the error.
// Used for every exchange whose response follows the standard SID+0x40
// positive-response convention. The no-explicit-client overload uses this
// family's own 0x7e1/0x7e9 `ctx.uds`; six connect-sequence exchanges instead
// pass `ctx.other_uds` explicitly (see the comment above connect_bootloader's
// session/seed block).
Result<bytes::Bytes> fatal_request(Ctx& ctx, uds::UdsClient& client, bytes::ByteView pdu, std::string_view operation)
{
    return ::fastecu::flash::fatal_request(UdsExchangeContext{client, kExchangePolicy, ctx.cancellation, ctx.events},
                                           pdu, kRejectionPrefix, operation);
}

Result<bytes::Bytes> fatal_request(Ctx& ctx, bytes::ByteView pdu, std::string_view operation)
{
    return fatal_request(ctx, ctx.uds, pdu, operation);
}

// The fatal_request + expected-response-prefix check every ctx.uds exchange
// below repeats -- see uds_client_exchange_common.h's fatal_query for what
// expected_prefix, subject, and min_payload_size mean.
Result<bytes::Bytes> fatal_query(Ctx& ctx, uds::UdsClient& client, bytes::ByteView pdu, bytes::ByteView expected_prefix,
                                 std::string_view subject, std::optional<std::size_t> min_payload_size = std::nullopt)
{
    return ::fastecu::flash::fatal_query(UdsExchangeContext{client, kExchangePolicy, ctx.cancellation, ctx.events}, pdu,
                                         expected_prefix, kRejectionPrefix, subject, min_payload_size);
}

Result<bytes::Bytes> fatal_query(Ctx& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix,
                                 std::string_view subject, std::optional<std::size_t> min_payload_size = std::nullopt)
{
    return fatal_query(ctx, ctx.uds, pdu, expected_prefix, subject, min_payload_size);
}

// Legacy's TCU ID / CAL ID queries and the second session request (lines
// 137-211, 244-265): logged on mismatch or absence but never halt
// connect_bootloader -- even a genuine exchange failure is logged and
// swallowed, mirroring subaru_hitachi_m32r_can_executor.cpp's
// non_fatal_query and legacy's own total absence of an early return here.
// All three follow the standard SID+0x40 convention (0xAA->0xEA, 0x09->0x49,
// 0x10->0x50), so they go through `client` (always ctx.other_uds here) like
// every other exchange, instead of a hand-rolled raw path.
void non_fatal_query(Ctx& ctx, uds::UdsClient& client, bytes::ByteView pdu,
                     std::optional<bytes::Byte> expected_subfunction, std::string_view label)
{
    ::fastecu::flash::non_fatal_query(UdsExchangeContext{client, kExchangePolicy, ctx.cancellation, ctx.events}, pdu,
                                      expected_subfunction, kRejectionPrefix, label);
}

// Legacy connect_bootloader, lines 87-408.
Status connect_bootloader(Ctx& ctx)
{
    // Kernel-alive pre-check (lines 100-129): a match short-circuits the
    // rest of connect_bootloader with zero further writes. Sent/read on
    // this family's own 0x7e1/0x7e9 pair, so this goes through the channel
    // directly (mirrors subaru_hitachi_m32r_can_executor.cpp's OBK-probe
    // pattern): a genuine channel-level Result failure still propagates;
    // only "no frame" or "frame present but content doesn't match" falls
    // through to full initialization, matching legacy's total absence of an
    // early return on that branch.
    info(ctx, "Checking if kernel is already running...");
    if (const Status sent = ctx.channel.send(
            bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStop, 0x02, 0x01}, ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> alive = ctx.channel.receive(200, ctx.cancellation);
    if (!alive.has_value())
    {
        return std::unexpected(alive.error());
    }
    if (alive->has_value() && alive->value().size() > 3 && (**alive)[0] == 0x71 &&
        (**alive)[1] == uds::kRoutineControlStop && (**alive)[2] == 0x02 && (**alive)[3] == 0x03)
    {
        info(ctx, "Kernel already running");
        return {};
    }
    if (alive->has_value())
    {
        error(ctx, std::format("Wrong response from TCU: {}", bytes::toHex(**alive)));
    }
    else
    {
        error(ctx, "No valid response from ECU");
    }

    info(ctx, "TCU Init...");

    // TCU ID / CAL ID queries (lines 137-211): sent on 0x7E0, non-fatal.
    // Six of connect_bootloader's exchanges (these two, both session
    // requests, and both seed exchanges below) are sent on 0x7E0 -- the OBD
    // generic-ECU envelope, NOT this family's own 0x7E1/0x7E9 pair `ctx.uds`
    // is bound to. Confirmed directly against lines 138-333: only the
    // initial alive probe, the jump, and the final alive re-check use 0x7E1
    // (the brief calls out the two identity queries explicitly; the
    // session/seed exchanges carry the same 0xE0 envelope byte on inspection
    // of the same line range). All six follow the standard SID+0x40
    // convention, so `ctx.other_uds` -- a second CanFlashUdsChannel/UdsClient
    // pair bound to (0x7e0, family.response_id) over the same underlying
    // transport (see execute() and Ctx's comment) -- carries them through
    // the same fatal_request()/non_fatal_query() machinery every other
    // exchange in this file uses, rather than a hand-rolled raw path.
    info(ctx, "Requesting TCU ID");
    non_fatal_query(ctx, ctx.other_uds, bytes::Bytes{uds::kSidEcuIdQuery}, std::nullopt, "TCU ID");
    info(ctx, "Requesting CAL ID");
    non_fatal_query(ctx, ctx.other_uds, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId},
                    uds::kVehicleInfoPidCalId, "CAL ID");

    info(ctx, "Initializing bootloader...");

    // Session 0x10/0x03 (lines 216-242): sent on 0x7E0, fatal.
    info(ctx, "Requesting session mode");
    Result<bytes::Bytes> session = fatal_query(
        ctx, ctx.other_uds, bytes::Bytes{uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic},
        bytes::Bytes{uds::kSessionExtendedDiagnostic}, "session mode request");
    if (!session.has_value())
    {
        return std::unexpected(session.error());
    }

    // Session 0x10/0x43 (lines 244-265): sent on 0x7E0, non-fatal.
    non_fatal_query(ctx, ctx.other_uds, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionBootload},
                    kSessionBootload, "session mode (bootloader)");

    // Seed request 0x27/0x01 (lines 267-293): sent on 0x7E0, fatal.
    info(ctx, "Starting seed request...");
    Result<bytes::Bytes> seed_reply =
        fatal_query(ctx, ctx.other_uds, bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed},
                    bytes::Bytes{uds::kSecurityAccessRequestSeed}, "seed request", 5);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    info(ctx, "Seed request ok");
    const bytes::ByteView seed = uds::payload(*seed_reply).subspan(1, 4);
    const bytes::Bytes key = seed_key(seed);

    // Seed key 0x27/0x02 (lines 305-333): sent on 0x7E0, fatal.
    info(ctx, "Sending seed key");
    bytes::Bytes key_request{uds::kSidSecurityAccess, uds::kSecurityAccessSendKey};
    key_request.insert(key_request.end(), key.begin(), key.end());
    Result<bytes::Bytes> key_reply =
        fatal_query(ctx, ctx.other_uds, key_request, bytes::Bytes{uds::kSecurityAccessSendKey}, "seed key");
    if (!key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    info(ctx, "Seed key ok");

    // Jump 0x10/0x02 (lines 339-365): back to this family's own 0x7e1,
    // fatal, standard SID+0x40 -- routed through fatal_request/ctx.uds.
    info(ctx, "Jumping to onboard kernel...");
    Result<bytes::Bytes> jump_reply =
        fatal_query(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, uds::kSessionProgramming},
                    bytes::Bytes{uds::kSessionProgramming}, "kernel jump");
    if (!jump_reply.has_value())
    {
        return std::unexpected(jump_reply.error());
    }
    info(ctx, "Jump to kernel ok");

    // Alive re-check 0x31/0x02/0x02/0x01 (lines 373-401): 0x7e1, fatal.
    info(ctx, "Checking if jump successful and kernel alive...");
    Result<bytes::Bytes> recheck =
        fatal_query(ctx, bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStop, 0x02, 0x01},
                    bytes::Bytes{uds::kRoutineControlStop, 0x02, 0x03}, "kernel alive re-check");
    if (!recheck.has_value())
    {
        return std::unexpected(recheck.error());
    }

    info(ctx, "Kernel verified to be running");
    return {};
}

// Legacy read_mem, lines 499-719 (the resolved kWindow, not the underflowed
// literal address -- see the plan's kReadRegion comment).
Result<bytes::Bytes> dump_flash_range(Ctx& ctx, PhaseReporter& progress)
{
    constexpr std::uint32_t kPageSize = 0x100;

    // "Settting dump start & length..." (lines 526-562): fatal on mismatch
    // or empty reply -- unlike subaru_hitachi_m32r_can_executor.cpp's
    // analogous step, this one IS fatal in this family's legacy source
    // (both branches of the length check return STATUS_ERROR).
    info(ctx, "Settting dump start & length...");
    Result<bytes::Bytes> setup =
        fatal_query(ctx, composeBe(uds::kSidRequestDownload, 0x04_b, 0x33_b, u24(kWindow.start), u24(kWindow.length)),
                    bytes::Bytes{0x20, 0x01, 0x04}, "dump start & length setup");
    if (!setup.has_value())
    {
        return std::unexpected(setup.error());
    }

    info(ctx, "Start reading ROM, please wait...");
    bytes::Bytes rom;
    rom.reserve(kWindow.length);
    for (std::uint32_t addr = kWindow.start; addr < kWindow.start + kWindow.length; addr += kPageSize)
    {
        // Legacy stopRequested() at line 584, top of loop.
        if (ctx.cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "read cancelled");
        }

        // Lines 567-576/601-623: SID 0xB7 + 3-byte big-endian address.
        Result<bytes::Bytes> chunk = fatal_request(ctx, composeBe(uds::kSidReadMemoryChunk, u24(addr)),
                                                   std::format("the flash read at 0x{:x}", addr));
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        const bytes::Bytes decrypted = decrypt_page(uds::payload(*chunk));
        rom.insert(rom.end(), decrypted.begin(), decrypted.end());
        progress.update(static_cast<int>(addr + kPageSize - kWindow.start));
    }

    info(ctx, "ROM read complete");

    // "Sending stop command..." (lines 673-700). Legacy's own check here is
    // inverted (received.at(4) == 0x77 -- a MATCHING response -- is treated
    // as the error branch), the opposite polarity of every other close/stop
    // check in this file (e.g. reflash_block's, lines 926-941, correctly
    // uses != 0x77). Reproducing the inversion literally would make a
    // successful read always report failure, defeating this port's stated
    // purpose of giving the family a working path for the first time.
    // Routing this exchange through fatal_request(), exactly like every
    // other standard SID+0x40 exchange in this file, naturally yields the
    // correct polarity (a matching 0x77 is Ok, anything else is an error)
    // without reproducing the bug -- disclosed here as a third deliberate
    // divergence, alongside the dead-code and address-clamp ones already
    // named in the plan and the matrix.
    if (Result<bytes::Bytes> stop = fatal_request(ctx, bytes::Bytes{uds::kSidRequestTransferExit}, "the stop command");
        !stop.has_value())
    {
        return std::unexpected(stop.error());
    }

    return rom;
}

// Legacy erase_mem, lines 991-1039.
Status erase_memory(Ctx& ctx)
{
    info(ctx, "Erasing TCU ROM...");
    if (const Status sent = ctx.channel.send(
            bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStop, 0x01, 0xff, 0xff, 0xff, 0xff},
            ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    // Legacy checks the echoed SID literally (0x31), not the standard
    // SID+0x40 positive-response convention (0x71) every other exchange in
    // this file uses -- this cannot go through fatal_request()/ctx.uds,
    // which would classify a raw 0x31-prefixed reply as Malformed and
    // reject it regardless of content (confirmed against lines 1022-1036;
    // the brief's own scripting note, "fatal-checked 0x31 0x02 0x01",
    // matches).
    Result<std::optional<bytes::Bytes>> received = ctx.channel.receive(2000, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(
            report_exchange_failure(ctx, received.error(), "Wrong response from TCU: ", "the erase command"));
    }
    if (!received->has_value() || received->value().size() < 3 || (**received)[0] != uds::kSidRoutineControl ||
        (**received)[1] != uds::kRoutineControlStop || (**received)[2] != 0x01)
    {
        if (received->has_value())
        {
            error(ctx, std::format("Wrong response from TCU: {}", bytes::toHex(**received)));
        }
        else
        {
            error(ctx, "No valid response from ECU");
        }
        return fail(ErrorKind::BadResponse, "flash area erase failed");
    }
    info(ctx, "Flash erased! Starting flash write, do not power off!");
    return {};
}

// Legacy reflash_block, lines 811-984, called once per modified block
// (blocks 3-10 of M32R_512KB's 11 -- see kWriteBlocks).
Status unlock_and_reflash_block(Ctx& ctx, bytes::ByteView block_plain, std::uint32_t start, std::uint32_t length,
                                std::uint32_t done_before, PhaseReporter& progress)
{
    constexpr std::uint32_t kChunkSize = 128;

    // "Settting flash start & length..." (lines 845-880): fatal, and only
    // the SID is checked (received.at(4) != 0x74) -- fatal_request()'s own
    // SID+0x40 matching already enforces exactly that.
    info(ctx, "Settting flash start & length...");
    if (Result<bytes::Bytes> setup =
            fatal_request(ctx, composeBe(uds::kSidRequestDownload, 0x04_b, 0x33_b, u24(start), u24(length)),
                          "the flash start & length setup");
        !setup.has_value())
    {
        return std::unexpected(setup.error());
    }

    const bytes::Bytes encrypted = encrypt_rom(block_plain);

    for (std::uint32_t offset = 0; offset < length; offset += kChunkSize)
    {
        // Legacy's stopRequested() check (line 884) returns 0 -- success --
        // to the caller on cancellation, which write_mem then reports as a
        // completed block; the same bug subaru_hitachi_m32r_can_executor.cpp
        // already disclosed and diverged from for its own family. Not
        // reproduced here either: a cancellation mid-write is reported as
        // Cancelled, never success.
        if (ctx.cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "write cancelled");
        }

        const std::uint32_t addr = start + offset;
        const bytes::ByteView chunk_data = bytes::ByteView(encrypted).subspan(offset, kChunkSize);
        // Legacy reads but never inspects the per-chunk reply at all (lines
        // 907-908) -- only a transport-level failure from the read itself
        // fails this exchange. Routed through the channel directly (not
        // fatal_request()/ctx.uds, which would enforce the SID+0x40 content
        // match this family's legacy code never checks here).
        if (const Status sent =
                ctx.channel.send(composeBe(uds::kSidWriteMemoryChunk, u24(addr), chunk_data), ctx.cancellation);
            !sent.has_value())
        {
            return sent;
        }
        if (Result<std::optional<bytes::Bytes>> reply = ctx.channel.receive(2000, ctx.cancellation); !reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        progress.update(static_cast<int>(done_before + offset + kChunkSize));
    }

    // Closing (lines 914-941): a single fatal attempt -- unlike Task 1's
    // ECU family (subaru_hitachi_m32r_can_executor.cpp), which retries up
    // to 6 times and tolerates every non-0x77 reply including a genuine
    // exchange failure, this family's own close check has no retry loop at
    // all.
    info(ctx, "Closing out Flashing of this block...");
    if (Result<bytes::Bytes> closed =
            fatal_request(ctx, bytes::Bytes{uds::kSidRequestTransferExit}, "the close command");
        !closed.has_value())
    {
        return std::unexpected(closed.error());
    }

    // "Verifying checksum..." (lines 945-978).
    info(ctx, "Verifying checksum...");
    Result<bytes::Bytes> checksum =
        fatal_query(ctx, bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStop, 0x02, 0x01},
                    bytes::Bytes{uds::kRoutineControlStop, 0x02}, "checksum verify");
    if (!checksum.has_value())
    {
        return std::unexpected(checksum.error());
    }
    info(ctx, "Checksum verified");
    return {};
}

// Legacy write_mem, lines 727-804: erase_mem() is a separate, explicit call
// here (unlike Task 1's ECU family, which has no standalone erase), followed
// by one reflash_block() call per modified block.
Status write_mem(Ctx& ctx, bytes::ByteView image, PhaseSequence& phases)
{
    PhaseReporter erase = phases.start("Erase", 1);
    if (const Status erased = erase_memory(ctx); !erased.has_value())
    {
        return erased;
    }
    erase.complete();

    PhaseReporter write = phases.start("Write ROM", static_cast<int>(kWindow.length));
    std::uint32_t done = 0;
    for (const MemoryRegion& block : kWriteBlocks)
    {
        const bytes::ByteView block_plain = image.subspan(block.start, block.length);
        if (const Status written = unlock_and_reflash_block(ctx, block_plain, block.start, block.length, done, write);
            !written.has_value())
        {
            return written;
        }
        done += block.length;
    }
    write.complete();
    return {};
}

} // namespace

Result<FlashExecutionResult> SubaruTcuCvtHitachiM32rCanExecutor::execute(const FlashPlan& plan,
                                                                         IFlashTransport& transport, IClock& clock,
                                                                         const ICancellationToken& cancellation,
                                                                         IEventSink& events)
{
    if (const Status matched =
            check_family_transport_match(plan, FlashFamily::SubaruTcuCvtHitachiM32rCan, TransportKind::CanIso15765);
        !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = validate_subaru_tcu_cvt_hitachi_m32r_can_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruTcuCvtHitachiM32rCanPlan>(plan.family_plan());
    Result<ICanFlashTransport *> can_transport =
        open_can_iso15765_transport(transport, Iso15765Config{
                                                   .bitrate = family.bitrate,
                                                   .request_id = family.request_id,
                                                   .response_id = family.response_id,
                                                   .extended_id = family.extended_id,
                                               });
    if (!can_transport.has_value())
    {
        return std::unexpected(can_transport.error());
    }
    ICanFlashTransport *can = *can_transport;

    const bool read = plan.operation() == FlashOperation::Read;
    PhaseSequence phases(events, read ? 2 : 3);
    PhaseReporter connect = phases.start(read ? "Connect to TCU" : "Connect", 1);

    CanFlashUdsChannel channel(*can, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);

    // Second channel/client pair for the six connect-sequence exchanges sent
    // on kOtherRequestId (0x7e0) instead of this family's own request_id
    // (0x7e1) -- see Ctx's comment and connect_bootloader's session/seed
    // block. Shares the same underlying *can transport as `channel`/
    // `uds_client`; both pairs are used strictly sequentially.
    CanFlashUdsChannel other_channel(*can, kOtherRequestId, family.response_id);
    uds::UdsClient other_uds_client(other_channel, clock, events);

    Ctx ctx{cancellation, events, clock, uds_client, other_uds_client, channel};

    info(ctx, "Connecting to Subaru TCU Hitachi CAN bootloader, please wait...");
    if (const Status connected = connect_bootloader(ctx); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connect.complete();

    if (plan.operation() == FlashOperation::Read)
    {
        events.notice("Reading ROM, please wait...");
        info(ctx, "Reading ROM from TCU Subaru Hitachi using CAN");

        PhaseReporter read_phase = phases.start("Read ROM", static_cast<int>(plan.transfer_region().length));
        Result<bytes::Bytes> window = dump_flash_range(ctx, read_phase);
        if (!window.has_value())
        {
            return std::unexpected(window.error());
        }
        read_phase.complete();

        // Legacy pads the unread low 0x8000 region with 0x00 (lines
        // 705-712) -- unlike MH8111/MH8104 (Tasks 4-5), which pad with
        // 0xFF; see the plan's kReadRegion comment and the umbrella's
        // per-family read-padding note.
        bytes::Bytes rom(kWindow.start, 0x00);
        rom.insert(rom.end(), window->begin(), window->end());
        return FlashExecutionResult{
            .operation = FlashOperation::Read,
            .read_bytes = std::move(rom),
        };
    }

    // build_subaru_tcu_cvt_hitachi_m32r_can_plan refuses TestWrite; the
    // guard is repeated here so a plan built another way cannot turn a dry
    // run into a real erase and write.
    if (plan.operation() != FlashOperation::Write)
    {
        return fail(ErrorKind::Unsupported,
                    "test_write is not supported by the Subaru TCU CVT Hitachi M32R CAN family");
    }

    events.notice("Writing ROM, please wait...");
    info(ctx, "Writing ROM to Subaru TCU Hitachi using CAN");
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
