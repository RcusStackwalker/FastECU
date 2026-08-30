#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor.h"

#include <algorithm>
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
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh72543_can_diesel_operation.cpp
// it was transcribed from.
namespace fastecu::flash
{
namespace
{
using bytes::composeBe;

// Legacy's three read timeouts, kept apart rather than flattened into one
// policy: they are this family's own wire timing (header lines 46-52). Where
// its 1N83M 1.5M and SH72531 siblings read with the 200 ms
// serial_read_short_timeout, this family mostly reads with the 2000 ms
// serial_read_timeout -- see the per-exchange citations below. The 2000 in
// read_memory's page loop is a bare literal (line 947), which is why the
// values, not the names, are authoritative here.
constexpr int kShortTimeoutMs = 200;   // serial_read_short_timeout
constexpr int kReceiveTimeoutMs = 500; // receive_timeout
constexpr int kLongTimeoutMs = 2000;   // serial_read_timeout
constexpr uds::ExchangePolicy kShortPolicy{.read_timeout_ms = kShortTimeoutMs};
constexpr uds::ExchangePolicy kReceivePolicy{.read_timeout_ms = kReceiveTimeoutMs};
constexpr uds::ExchangePolicy kLongPolicy{.read_timeout_ms = kLongTimeoutMs};

// Session ids in ISO 14229-1's 0x40-0x5F vehicle-manufacturer-specific band;
// legacy uses its own values here rather than the standard subfunctions.
constexpr bytes::Byte kSessionProbe = 0x5F;     // OBK probe / access-method probe
constexpr bytes::Byte kSessionInCarOpen = 0x63; // in-car "session open" subfunction
constexpr bytes::Byte kSessionBench = 0x43;     // bench programming session
constexpr bytes::Byte kSessionBenchJump = 0x42; // bench jump to on-board kernel
constexpr bytes::Byte kSessionInCarJump = 0x62; // in-car jump to on-board kernel
// Sent to 0x7A2 only (line 375-383); legacy names it by its value alone, and
// nothing in the source says what the addressed module does with it.
constexpr bytes::Byte kSessionVendorC0 = 0xC0;

// SecurityAccess subfunctions: ISO 14229-1 pairs an odd requestSeed with the
// next even sendKey for the same level. This family uses level 0x61/0x62, not
// the 0x01/0x02 level uds_service_ids.h names.
constexpr bytes::Byte kSecurityAccessRequestSeed = 0x61;
constexpr bytes::Byte kSecurityAccessSendKey = 0x62;

// Positive-response service ids for the SIDs this file inspects without going
// through UdsClient (which does the SID + 0x40 arithmetic itself).
constexpr bytes::Byte kSessionControlReply = uds::kSidDiagnosticSessionControl + 0x40;   // 0x50
constexpr bytes::Byte kReadDataByIdentifierReply = uds::kSidReadDataByIdentifier + 0x40; // 0x62
constexpr bytes::Byte kRoutineControlReply = uds::kSidRoutineControl + 0x40;             // 0x71

// ISO 14229-1 services used only by the in-car arm's fire-and-forget run
// (legacy lines 429-483). Kept local per uds_service_ids.h's rule that a
// value no other family in that header's list sends stays with its own
// executor.
constexpr bytes::Byte kSidControlDtcSetting = 0x85;    // ISO 14229-1
constexpr bytes::Byte kSidCommunicationControl = 0x28; // ISO 14229-1

// Routine identifier for both the erase (0x02 0x01) and the checksum verify
// (0x02 0x02) routines; vendor-assigned, so it stays here rather than in the
// shared UDS header.
constexpr bytes::Byte kRoutineIdHigh = 0x02;
constexpr bytes::Byte kRoutineErase = 0x01;
constexpr bytes::Byte kRoutineChecksum = 0x02;

// RequestDownload/RequestUpload format bytes: 0x04 dataFormatIdentifier and
// 0x44 addressAndLengthFormatIdentifier (4-byte address, 4-byte length).
constexpr bytes::Byte kDataFormatIdentifier = 0x04;
constexpr bytes::Byte kAddressAndLengthFormat = 0x44;

// ISO 14229-1 dataIdentifier 0xF182 (systemSupplierECUSoftwareNumber band).
// Unlike its three siblings, which query the ECU id with the vendor SID 0xAA,
// this family reads it with a standard ReadDataByIdentifier (lines 131-140)
// and checks the whole echoed `62 F1 82` header (line 146).
constexpr bytes::Byte kDidEcuIdHigh = 0xF1;
constexpr bytes::Byte kDidEcuIdLow = 0x82;

// Byte 0 of the plan image is this flash address. Legacy write_memory hands
// reflash_block &data_array[fdt->fblocks->start] (line 1129), i.e. the
// encrypted whole-ROM buffer offset by fblocks[0].start = 0x8000, and
// reflash_block then indexes newdata[i + blockctr * blocksize] (line 1217) --
// which together read data_array[blockaddr + i]. See reflash_block below.
constexpr std::uint32_t kImageStart = 0x00000000;
constexpr std::uint32_t kPageSize = 0x100;

// Additional CAN request ids the in-car arm addresses (legacy lines 375-483).
// 0x7DF is ISO 15765-4's functional-broadcast id; the other three are
// physical ids of other modules, which legacy names only by their numbers.
constexpr std::uint32_t kInCarIdA2 = 0x7a2;
constexpr std::uint32_t kInCarIdFunctional = 0x7df;
constexpr std::uint32_t kInCarIdE1 = 0x7e1;
constexpr std::uint32_t kInCarIdB0 = 0x7b0;

constexpr auto kSeedKeyTable =
    std::to_array<std::uint16_t>({0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71, 0xB046, 0x7F4A,
                                  0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B});
constexpr auto kEncryptTable = std::to_array<std::uint16_t>({0xC85B, 0x32C0, 0xE282, 0x92A0});
constexpr auto kDecryptTable = std::to_array<std::uint16_t>({0x92A0, 0xE282, 0x32C0, 0xC85B});
constexpr auto kIndexTransformation =
    std::to_array<std::uint8_t>({0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
                                 0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8});

bytes::Bytes seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable, kIndexTransformation);
}

bytes::Bytes encrypt_rom(bytes::ByteView image)
{
    return SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()), kEncryptTable,
                                         kIndexTransformation);
}

// Legacy decrypts the whole accumulated dump in one call (line 1047);
// SsmProtocol::calculatePayload transforms independent 4-byte words, so
// decrypting each 256-byte page as it arrives produces byte-identical output
// without a second full-ROM buffer.
bytes::Bytes decrypt_page(bytes::ByteView page)
{
    return SsmProtocol::calculatePayload(page, static_cast<std::uint32_t>(page.size()), kDecryptTable,
                                         kIndexTransformation);
}

// Most exchanges go through UdsClient over CanFlashUdsChannel. The
// exceptions are the probes and fire-and-forget writes whose reply legacy
// either tolerates or never inspects at all: UdsClient enforces the
// SID+0x40 convention and decodes NRCs, so those go through the channel --
// or, where legacy does not even validate the reply's arbitration id,
// through the transport -- directly. `channel` is the same
// CanFlashUdsChannel instance `uds` wraps.
struct Ctx
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    IClock& clock;
    uds::UdsClient& uds;
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

constexpr std::string_view kRejectionPrefix = "Wrong response from ECU: ";

UdsExchangeContext exchange_context(Ctx& ctx, const uds::ExchangePolicy& policy)
{
    return UdsExchangeContext{ctx.uds, policy, ctx.cancellation, ctx.events};
}

// The "fatal" shape every UdsClient-backed exchange below uses. See
// uds_client_exchange_common.h for the shared rejection/cancellation logging
// this delegates to.
Result<bytes::Bytes> fatal_request(Ctx& ctx, bytes::ByteView pdu, const uds::ExchangePolicy& policy,
                                   std::string_view operation)
{
    return ::fastecu::flash::fatal_request(exchange_context(ctx, policy), pdu, kRejectionPrefix, operation);
}

// fatal_request plus the expected-response-prefix check -- see
// uds_client_exchange_common.h's fatal_query for what expected_prefix,
// subject and min_payload_size mean.
Result<bytes::Bytes> fatal_query(Ctx& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix,
                                 const uds::ExchangePolicy& policy, std::string_view subject,
                                 std::optional<std::size_t> min_payload_size = std::nullopt)
{
    return ::fastecu::flash::fatal_query(exchange_context(ctx, policy), pdu, expected_prefix, kRejectionPrefix, subject,
                                         min_payload_size);
}

// Legacy's four non-fatal identity queries (ECU ID/VIN/CAL ID/CVN, lines
// 131-282): each is info-logged on a matching reply and error-logged
// otherwise, but the connect sequence never halts here -- even a genuine
// exchange failure is logged and swallowed, mirroring legacy's total absence
// of early returns in that block. All four read with serial_read_timeout
// (lines 143, 186, 219, 256).
void non_fatal_query(Ctx& ctx, bytes::ByteView pdu, std::optional<bytes::Byte> expected_subfunction,
                     std::string_view label)
{
    ::fastecu::flash::non_fatal_query(exchange_context(ctx, kLongPolicy), pdu, expected_subfunction, kRejectionPrefix,
                                      label);
}

// The ECU ID query needs a two-byte reply check (`F1 82`, line 146) where the
// shared non_fatal_query checks one subfunction byte, so this family carries
// its own non-fatal variant. Same shape otherwise: log on a match, log and
// continue on anything else.
void non_fatal_did_query(Ctx& ctx, bytes::ByteView pdu, bytes::ByteView expected_did, std::string_view label)
{
    Result<bytes::Bytes> reply = ctx.uds.request(pdu, kLongPolicy, ctx.cancellation);
    if (!reply.has_value())
    {
        error(ctx, std::format("{}{}", kRejectionPrefix, reply.error().detail));
        return;
    }
    const bytes::ByteView payload = uds::payload(*reply);
    if (payload.size() < expected_did.size() || !std::equal(expected_did.begin(), expected_did.end(), payload.begin()))
    {
        error(ctx, std::format("{}unexpected {} response", kRejectionPrefix, label));
        return;
    }
    // Legacy trims the raw frame with remove(0, 7) -- the 4-byte envelope plus
    // `62 F1 82` -- and keeps everything after it (line 149; its
    // remove(5, len-5) truncation to 5 bytes is commented out at line 150).
    info(ctx, std::format("{}: {}", label, bytes::toHex(payload.subspan(expected_did.size()))));
}

// Legacy's "log a mismatch, abort only on an absent or too-short reply"
// probe shape (lines 286-312, 314-340, 346-372, 655-681). fatal_query aborts
// on a mismatch and non_fatal_query never aborts, so neither fits; this goes
// through the channel directly and returns the envelope-stripped frame so a
// caller can read further bytes out of it. Every one of this family's four
// tolerant probes reads with serial_read_timeout (lines 297, 324, 357, 663).
Result<bytes::Bytes> tolerant_probe(Ctx& ctx, bytes::ByteView pdu, bytes::Byte expected_service,
                                    bytes::Byte expected_subfunction, std::string_view subject)
{
    if (const Status sent = ctx.channel.send(pdu, ctx.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    Result<std::optional<bytes::Bytes>> received = ctx.channel.receive(kLongTimeoutMs, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    // Legacy requires received.length() > 5, i.e. at least two bytes past the
    // 4-byte envelope; anything shorter takes its "No valid response from
    // ECU" path and returns STATUS_ERROR.
    if (!received->has_value() || received->value().size() < 2)
    {
        error(ctx, "No valid response from ECU");
        return fail(ErrorKind::Timeout, std::format("no response from ECU during the {}", subject));
    }
    const bytes::Bytes& frame = **received;
    if (frame[0] != expected_service || frame[1] != expected_subfunction)
    {
        error(ctx, std::format("{}{}", kRejectionPrefix, bytes::toHex(frame)));
    }
    return frame;
}

// Legacy's in-car fire-and-forget exchange (lines 375-483): write on `id`,
// read whatever arrives next, discard it. Legacy validates neither the
// reply's arbitration id nor its content, so the reply is taken from the
// transport directly -- a CanFlashUdsChannel bound to `id` would impose a
// reply-id check legacy does not have. The channel is still used for the
// write, which is exactly its 4-byte-envelope job. `timeout_ms` is per
// exchange: this family reads the 0x7E0 `10 63` write with
// serial_read_timeout (line 394) and the other nine with
// serial_read_short_timeout.
Status fire_and_forget(Ctx& ctx, ICanFlashTransport& can, std::uint32_t request_id, bytes::ByteView pdu, int timeout_ms)
{
    // The reply is read from `can` below, never through this channel, so the
    // response-id slot is filled with the request id and never consulted.
    CanFlashUdsChannel channel(can, request_id, request_id);
    if (const Status sent = channel.send(pdu, ctx.cancellation); !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> ignored = can.read(timeout_ms, ctx.cancellation);
    if (!ignored.has_value())
    {
        return std::unexpected(ignored.error());
    }
    return {};
}

// The seed request / seed key pair both arms share (in-car lines 485-556,
// bench lines 682-753). Both are fatal on a mismatch and on an absent reply,
// and both read with serial_read_timeout (lines 496, 537, 691, 733). Unlike
// its SH72531 sibling this family logs "Starting seed request" in both arms
// already (lines 485 and 682), so no log divergence is needed here.
Status security_access(Ctx& ctx)
{
    info(ctx, "Starting seed request");
    Result<bytes::Bytes> seed_reply =
        fatal_query(ctx, bytes::Bytes{uds::kSidSecurityAccess, kSecurityAccessRequestSeed},
                    bytes::Bytes{kSecurityAccessRequestSeed}, kLongPolicy, "seed request", 5);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    info(ctx, "Seed request ok");
    // Legacy reads the four seed bytes at raw frame offsets 6-9, i.e. payload
    // offsets 1-4 once the 4-byte envelope and the service id are stripped
    // (lines 517-520, 712-715).
    const bytes::Bytes key = seed_key(uds::payload(*seed_reply).subspan(1, 4));

    info(ctx, "Sending seed key");
    Result<bytes::Bytes> key_reply = fatal_query(ctx, composeBe(uds::kSidSecurityAccess, kSecurityAccessSendKey, key),
                                                 bytes::Bytes{kSecurityAccessSendKey}, kLongPolicy, "seed key");
    if (!key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    info(ctx, "Seed key ok");
    return {};
}

// Legacy's kernel jump plus its bounded re-read loop (in-car lines 617-648,
// try_count < 10; bench lines 755-790, try_count < 50). Both arms read once
// before the loop, with serial_read_timeout (lines 629, 770); inside the loop
// the in-car arm keeps reading with serial_read_timeout (line 643) while the
// bench arm drops to serial_read_short_timeout (line 785), which is why
// `loop_timeout_ms` is a parameter. The loop's `init_ready` flag is never read
// after the loop and both arms fall straight through to
// `return STATUS_SUCCESS` (line 796), so an unacknowledged jump is logged
// here but is not an error -- deliberately not "fixed".
Status jump_to_kernel(Ctx& ctx, bytes::Byte session, int max_tries, int loop_timeout_ms)
{
    info(ctx, "Jump to onboad kernel");
    if (const Status sent =
            ctx.channel.send(bytes::Bytes{uds::kSidDiagnosticSessionControl, session}, ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> received = ctx.channel.receive(kLongTimeoutMs, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    for (int attempt = 0; attempt < max_tries; ++attempt)
    {
        if (received->has_value() && received->value().size() > 1 && (**received)[0] == kSessionControlReply &&
            (**received)[1] == session)
        {
            info(ctx, "Kernel jump acknowledged");
            return {};
        }
        if (const Status slept = ctx.clock.sleep(100, ctx.cancellation); !slept.has_value())
        {
            return slept;
        }
        received = ctx.channel.receive(loop_timeout_ms, ctx.cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
    }
    error(ctx, "Kernel jump was not acknowledged; continuing, as legacy does");
    return {};
}

// In-car programming arm, legacy lines 342-649.
Status connect_in_car(Ctx& ctx, ICanFlashTransport& can)
{
    info(ctx, "In car programming: accessing, please wait...");
    // Line 345: this wait precedes the write below, so no read timeout
    // subsumes it.
    if (const Status slept = ctx.clock.sleep(500, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    // Lines 346-372: mismatch logs, absent reply aborts.
    if (Result<bytes::Bytes> probe = tolerant_probe(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe},
                                                    kSessionControlReply, 0x01, "in-car access-method probe");
        !probe.has_value())
    {
        return std::unexpected(probe.error());
    }

    // Lines 375-483: ten fire-and-forget writes across four extra CAN ids.
    // Every reply is read and discarded, so a wrong byte here is invisible
    // on the wire without the scripted test that pins it.
    const struct
    {
        std::uint32_t id;
        bytes::Bytes pdu;
        int timeout_ms;
    } fire_and_forget_run[] = {
        {kInCarIdA2, {uds::kSidDiagnosticSessionControl, kSessionVendorC0}, kShortTimeoutMs}, // lines 375-383
        // The one exchange in this run legacy reads with serial_read_timeout
        // (line 394); the other nine use serial_read_short_timeout.
        {0x7e0, {uds::kSidDiagnosticSessionControl, kSessionInCarOpen}, kLongTimeoutMs}, // lines 386-394
        {kInCarIdFunctional,
         {uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic},
         kShortTimeoutMs}, // lines 397-405
        {kInCarIdE1,
         {uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic},
         kShortTimeoutMs}, // lines 408-416
        {kInCarIdB0,
         {uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic},
         kShortTimeoutMs},                                                             // lines 419-427
        {kInCarIdB0, {kSidControlDtcSetting, 0x02}, kShortTimeoutMs},                  // lines 430-438
        {kInCarIdFunctional, {kSidControlDtcSetting, 0x02}, kShortTimeoutMs},          // lines 441-449
        {kInCarIdB0, {kSidControlDtcSetting, 0x02}, kShortTimeoutMs},                  // lines 452-460
        {kInCarIdFunctional, {kSidControlDtcSetting, 0x02}, kShortTimeoutMs},          // lines 463-471
        {kInCarIdFunctional, {kSidCommunicationControl, 0x03, 0x01}, kShortTimeoutMs}, // lines 474-483
    };
    for (const auto& exchange : fire_and_forget_run)
    {
        if (const Status sent = fire_and_forget(ctx, can, exchange.id, exchange.pdu, exchange.timeout_ms);
            !sent.has_value())
        {
            return sent;
        }
    }

    // Lines 485-556: seed/key on the primary 0x7E0 pair, fatal throughout.
    if (const Status unlocked = security_access(ctx); !unlocked.has_value())
    {
        return unlocked;
    }

    // Lines 558-585: unlike the earlier 0x10 0x5F probes, this one is fatal
    // on a mismatch, and expects subfunction 0x63 rather than 0x01.
    if (Result<bytes::Bytes> session =
            fatal_query(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe},
                        bytes::Bytes{kSessionInCarOpen}, kLongPolicy, "in-car session confirmation");
        !session.has_value())
    {
        return std::unexpected(session.error());
    }

    // Lines 587-615: the branch selector is re-read, this time fatally, and
    // its content past the 0x62 0x10 header is not inspected.
    if (Result<bytes::Bytes> selector =
            fatal_query(ctx, bytes::Bytes{uds::kSidReadDataByIdentifier, 0x10, 0x1D}, bytes::Bytes{0x10}, kLongPolicy,
                        "in-car programming-mode confirmation");
        !selector.has_value())
    {
        return std::unexpected(selector.error());
    }

    return jump_to_kernel(ctx, kSessionInCarJump, 10, kLongTimeoutMs);
}

// Bench programming arm, legacy lines 649-791.
Status connect_bench(Ctx& ctx)
{
    info(ctx, "Bench programming: accessing, please wait...");
    // Line 653. Its in-car counterpart waits 500 ms (line 345); this arm waits
    // 50, and both precede a write, so neither is subsumed by a read timeout.
    if (const Status slept = ctx.clock.sleep(50, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    // Lines 655-681. This is the family's one commented-out
    // `return STATUS_ERROR` (line 672): a wrong `50 43` answer is logged and
    // stepped over, and only an absent or too-short reply aborts. Its three
    // sibling families abort here, so this tolerance is transcribed
    // deliberately rather than normalized away.
    if (Result<bytes::Bytes> session =
            tolerant_probe(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionBench}, kSessionControlReply,
                           kSessionBench, "bench diagnostic session");
        !session.has_value())
    {
        return std::unexpected(session.error());
    }

    // Lines 682-753.
    if (const Status unlocked = security_access(ctx); !unlocked.has_value())
    {
        return unlocked;
    }

    return jump_to_kernel(ctx, kSessionBenchJump, 50, kShortTimeoutMs);
}

// Legacy connect_bootloader, lines 92-797. The legacy `is_serial_port_open()`
// guard (lines 98-104) has no equivalent: execute() below opens the transport
// through the port and propagates that Status, so an unopened bus fails
// earlier and with a typed error.
Status connect_bootloader(Ctx& ctx, ICanFlashTransport& can)
{
    // OBK-already-active probe (lines 108-128). A match short-circuits the
    // whole sequence; a mismatch OR an empty read is a non-fatal miss that
    // falls through to initializing the ECU, so this goes through the channel
    // directly rather than through ctx.uds. The only exchange in the connect
    // sequence that reads with serial_read_short_timeout (line 118).
    info(ctx, "Checking if OBK is active...");
    if (const Status sent =
            ctx.channel.send(bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe}, ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> obk = ctx.channel.receive(kShortTimeoutMs, ctx.cancellation);
    if (!obk.has_value())
    {
        return std::unexpected(obk.error());
    }
    if (obk->has_value() && obk->value().size() > 1 && (**obk)[0] == kSessionControlReply &&
        (**obk)[1] == kSessionProbe)
    {
        info(ctx, "OBK is active!");
        return {};
    }
    info(ctx, "OBK not active, initialising ECU...");

    // Lines 131-282: four non-fatal identity queries.
    info(ctx, "Requesting ECU ID");
    non_fatal_did_query(ctx, bytes::Bytes{uds::kSidReadDataByIdentifier, kDidEcuIdHigh, kDidEcuIdLow},
                        bytes::Bytes{kDidEcuIdHigh, kDidEcuIdLow}, "ECU ID");
    info(ctx, "Requesting VIN");
    non_fatal_query(ctx, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidVin}, uds::kVehicleInfoPidVin,
                    "VIN");
    info(ctx, "Requesting CAL ID");
    non_fatal_query(ctx, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId},
                    uds::kVehicleInfoPidCalId, "CAL ID");
    info(ctx, "Requesting CVN");
    non_fatal_query(ctx, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCvn}, uds::kVehicleInfoPidCvn,
                    "CVN");

    // Lines 284-312: mismatch logs, absent reply aborts.
    info(ctx, "Checking access method");
    if (Result<bytes::Bytes> access =
            tolerant_probe(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe}, kSessionControlReply,
                           0x01, "access-method probe");
        !access.has_value())
    {
        return std::unexpected(access.error());
    }

    // Lines 314-340: the branch selector, same tolerant shape.
    Result<bytes::Bytes> selector = tolerant_probe(ctx, bytes::Bytes{uds::kSidReadDataByIdentifier, 0x10, 0x1D},
                                                   kReadDataByIdentifierReply, 0x10, "programming-branch selector");
    if (!selector.has_value())
    {
        return std::unexpected(selector.error());
    }
    // Line 342 reads raw frame byte 7, i.e. payload byte 3 once the envelope
    // is stripped. Legacy reads it without a length check and would run past
    // the end of a short reply; this refuses instead of reading out of bounds.
    // This is a second, non-redundant guard: tolerant_probe above only
    // enforces a floor of 2 bytes (frame[0]/frame[1]), which does not cover
    // a 3-byte reply, so a length check for the byte-3 access needed here
    // still has to happen at this call site.
    if (selector->size() < 4)
    {
        error(ctx, "Wrong response from ECU: programming-branch selector reply is too short");
        return fail(ErrorKind::BadResponse, "programming-branch selector reply is too short");
    }
    return (*selector)[3] != 0xFF ? connect_in_car(ctx, can) : connect_bench(ctx);
}

// The 0x34 RequestDownload / 0x35 RequestUpload setup PDU. Unlike its three
// siblings, which emit these bytes as literals, this family already computes
// them from start_addr and length (lines 827-843, 866-882, 1400-1413); routing
// them through the region field produces the identical wire bytes its siblings
// spell out, and preserves this family's computed form.
bytes::Bytes setup_pdu(bytes::Byte service, const MemoryRegion& region)
{
    return composeBe(service, kDataFormatIdentifier, kAddressAndLengthFormat, region.start, region.length);
}

// Legacy's own log line for all three "start & length" announcements
// (lines 822-824, 1191-1193, 1396-1398). QString::number(x, 16) is lowercase
// and unpadded, so this reproduces it character for character.
std::string start_and_length_line(const MemoryRegion& region)
{
    return std::format("Setting flash start: 0x{:x} and length: 0x{:x}", region.start, region.length);
}

// Legacy read_memory, lines 800-1062. Its `start_addr = 0x8000` /
// `length = 0x001F7F00` overwrite is commented out (lines 813-814), so unlike
// its three siblings this family's execute() arguments -- fblocks[0].start and
// fblocks[0].len -- actually reach it. The plan carries exactly that region.
Result<bytes::Bytes> read_memory(Ctx& ctx, const SubaruDensoSh72543CanDieselPlan& family, const MemoryRegion& region,
                                 PhaseReporter& progress)
{
    // Lines 822-901: both setup exchanges are fatal on a mismatch and on an
    // absent reply, and both read with serial_read_short_timeout (lines 845,
    // 884) -- the two reads in this family that do not use the long timeout.
    info(ctx, start_and_length_line(region));
    if (Result<bytes::Bytes> download =
            fatal_query(ctx, setup_pdu(uds::kSidRequestDownload, region), bytes::Bytes{0x20, 0x01, 0x05}, kShortPolicy,
                        "dump start & length setup");
        !download.has_value())
    {
        return std::unexpected(download.error());
    }
    if (Result<bytes::Bytes> upload = fatal_query(ctx, setup_pdu(uds::kSidRequestUpload, region),
                                                  bytes::Bytes{0x20, 0x01, 0x01}, kShortPolicy, "dump upload setup");
        !upload.has_value())
    {
        return std::unexpected(upload.error());
    }

    info(ctx, "Start reading ROM, please wait...");
    bytes::Bytes rom;
    rom.reserve(region.length);
    for (std::uint32_t offset = 0; offset < region.length; offset += kPageSize)
    {
        // Legacy stopRequested() at line 924, top of loop.
        if (ctx.cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "read cancelled");
        }
        const std::uint32_t addr = region.start + offset;
        // Lines 907-967: SID 0xB7 plus a 4-byte address. Legacy fixes the
        // top byte at 0x00 (line 913) and patches only bytes 6-8 (lines
        // 941-943); every address in this region is below 0x1000000, so the
        // 4-byte big-endian encoding here is byte-identical. The reply is
        // 0xF7 plus one 256-byte encrypted page (lines 947-968), read with a
        // bare 2000 literal rather than a named constant.
        Result<bytes::Bytes> chunk = fatal_request(ctx, composeBe(uds::kSidReadMemoryChunk, addr), kLongPolicy,
                                                   std::format("the flash read at 0x{:x}", addr));
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        const bytes::Bytes decrypted = decrypt_page(uds::payload(*chunk));
        rom.insert(rom.end(), decrypted.begin(), decrypted.end());
        progress.update(static_cast<int>(offset + kPageSize));
        // Line 1001: this family paces each page by a millisecond before the
        // next 0xB7 write. Kept, since no read timeout subsumes a delay that
        // precedes a write.
        if (const Status slept = ctx.clock.sleep(1, ctx.cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }

    info(ctx, "ROM read complete");

    // Stop command (lines 1019-1045): up to six attempts, each re-sending
    // 0x37, and the loop's outcome is never checked -- legacy proceeds to
    // build the image whether or not the ECU ever answered 0x77.
    info(ctx, "Sending stop command");
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        if (Result<bytes::Bytes> stop =
                ctx.uds.request(bytes::Bytes{uds::kSidRequestTransferExit}, kReceivePolicy, ctx.cancellation);
            stop.has_value())
        {
            info(ctx, std::format("Stop request response: {}", bytes::toHex(*stop)));
            break;
        }
    }

    // Lines 1047-1058: the unread region below 0x8000 is synthesized as 0xFF
    // at offset 0 and a 0x100 0xFF tail is appended. Byte 0 of the returned
    // image is therefore address kImageStart = 0.
    bytes::Bytes image;
    image.reserve(family.lead_pad_len + rom.size() + family.tail_pad_len);
    image.assign(family.lead_pad_len, 0xFF);
    image.insert(image.end(), rom.begin(), rom.end());
    image.insert(image.end(), family.tail_pad_len, 0xFF);
    return image;
}

// Legacy erase_memory, lines 1375-1488. Unlike its siblings this one takes the
// block it is to erase -- `erase_memory(const flashdev_t *fdt, unsigned
// blockno)` -- and write_memory calls it as
// `erase_memory(&flashdevices[mcu_type_index], 0)` (line 1106), i.e. block 0,
// which is the plan's transfer region. Shape is otherwise the siblings': the
// setup PDU, the erase trigger, then a bounded re-read loop that never
// re-sends.
Status erase_memory(Ctx& ctx, const MemoryRegion& region)
{
    info(ctx, start_and_length_line(region));
    if (Result<bytes::Bytes> setup =
            fatal_query(ctx, setup_pdu(uds::kSidRequestDownload, region), bytes::Bytes{0x20, 0x01, 0x05},
                        kReceivePolicy, "flash start & length setup");
        !setup.has_value())
    {
        return std::unexpected(setup.error());
    }

    info(ctx, "Erasing ECU ROM");
    // Lines 1437-1450. Sent through the channel rather than UdsClient: legacy
    // does not read a reply here at all, and the loop below consumes the
    // ECU's answer instead.
    if (const Status sent = ctx.channel.send(bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart,
                                                          kRoutineIdHigh, kRoutineErase, 0xff, 0xff, 0xff, 0xff},
                                             ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    if (const Status slept = ctx.clock.sleep(500, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    for (int attempt = 0; attempt < 20; ++attempt)
    {
        Result<std::optional<bytes::Bytes>> received = ctx.channel.receive(kReceiveTimeoutMs, ctx.cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        if (received->has_value() && received->value().size() > 2 && (**received)[0] == kRoutineControlReply &&
            (**received)[1] == uds::kRoutineControlStart && (**received)[2] == kRoutineIdHigh)
        {
            info(ctx, "Flash erased! Starting flash write, do not power off!");
            return {};
        }
        if (const Status slept = ctx.clock.sleep(500, ctx.cancellation); !slept.has_value())
        {
            return slept;
        }
    }

    error(ctx, "Flash area erase failed");
    return fail(ErrorKind::BadResponse, "flash area erase failed");
}

// Legacy reflash_block, lines 1156-1368, called for block 0 only.
Status reflash_block(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseReporter& progress)
{
    constexpr std::uint32_t kChunkSize = 256;
    const std::uint32_t max_chunks = block.length / kChunkSize;

    // The whole image is encrypted once up front (legacy write_memory line
    // 1084), not per chunk.
    const bytes::Bytes encrypted = encrypt_rom(image);

    info(ctx, start_and_length_line(block));
    for (std::uint32_t chunk_index = 0; chunk_index < max_chunks; ++chunk_index)
    {
        // Legacy's stopRequested() check (line 1197) returns 0 -- success --
        // to the caller, which write_memory then reports as a completed
        // block. That is not reproduced: a cancellation mid-write must never
        // be reported as a successful ROM write, so this deliberately
        // diverges from the literal legacy return value, exactly as
        // subaru_hitachi_m32r_can_executor.cpp does.
        if (ctx.cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "write cancelled");
        }
        const std::uint32_t block_addr = block.start + chunk_index * kChunkSize;
        // Lines 1202-1218: SID 0xB6, a 4-byte address whose top byte legacy
        // pins to 0x00 (line 1210) -- identical to a big-endian encoding for
        // every address in this region -- and 256 encrypted bytes taken from
        // the absolute flash address.
        //
        // The write image is based at address 0 (kImageStart), which is what
        // legacy does too, though it takes two lines to see: write_memory
        // passes `&data_array[fdt->fblocks->start]` (line 1129) -- the
        // encrypted whole-ROM buffer offset by fblocks_SH72543d[0].start =
        // 0x8000 -- and reflash_block then indexes
        // `newdata[i + blockctr * blocksize]` (line 1217), a block-relative
        // index. The two compose to data_array[0x8000 + blockctr*256 + i],
        // i.e. data_array[block_addr + i]: the absolute address, matching
        // read_memory's own image base and the three sibling families.
        //
        // The wave-4 design's "write-base divergence" section reads line 1217
        // alone and concludes legacy wrote a full ROM 0x8000 low. Re-deriving
        // it with the caller's pre-offset shows it does not: this port is
        // byte-identical to legacy here, not a behavior change. Do not
        // "restore" a 0x8000 shift on the strength of that section.
        const std::size_t image_offset = block_addr - kImageStart;
        if (Result<bytes::Bytes> written =
                fatal_request(ctx,
                              composeBe(uds::kSidWriteMemoryChunk, block_addr,
                                        bytes::ByteView(encrypted).subspan(image_offset, kChunkSize)),
                              kReceivePolicy, std::format("the flash write at 0x{:x}", block_addr));
            !written.has_value())
        {
            return std::unexpected(written.error());
        }
        progress.update(static_cast<int>((chunk_index + 1) * kChunkSize));
    }

    // Close-block retry loop (lines 1282-1311): up to six attempts, tolerant
    // of any non-0x77 answer -- including a genuine exchange failure -- and
    // the loop's own `connected` flag is never read afterwards. Transcribed
    // exactly: no check is added here that legacy does not have.
    info(ctx, "Closing out Flashing of this block");
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        if (Result<bytes::Bytes> closed =
                ctx.uds.request(bytes::Bytes{uds::kSidRequestTransferExit}, kShortPolicy, ctx.cancellation);
            closed.has_value())
        {
            info(ctx, "Closed succesfully");
            break;
        }
    }

    // Line 1313: a settle before the checksum write, which no read timeout
    // subsumes.
    if (const Status slept = ctx.clock.sleep(100, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    // Checksum verify (lines 1315-1365). The ECU answers pending
    // (0x7F 0x31 0x78) before the real result; UdsClient absorbs that NRC by
    // re-reading internally, so this is a single request() call. Legacy
    // additionally *required* the pending answer to arrive first and failed
    // without it (lines 1329-1345); UdsClient accepts an immediate positive
    // answer too.
    info(ctx, "Verifying checksum");
    if (Result<bytes::Bytes> checksum = fatal_query(
            ctx,
            bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, kRoutineIdHigh, kRoutineChecksum, 0x01},
            bytes::Bytes{uds::kRoutineControlStart, kRoutineIdHigh}, kLongPolicy, "checksum verify");
        !checksum.has_value())
    {
        // Legacy pairs every checksum rejection with this second line
        // (lines 1333, 1343).
        info(ctx, "Checksum not verified");
        return std::unexpected(checksum.error());
    }
    info(ctx, "Checksum verified");
    return {};
}

// Legacy write_memory, lines 1070-1149. block_modified is {1} over
// numblocks == 1, so block 0 -- fblocks_SH72543d[0], the plan's transfer
// region -- is the one block erased and reflashed. Its three siblings all
// flash block 1 of a three-block table; this family's table has a single
// block because its 0x0-0x8000 entry is commented out.
Status write_memory(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseSequence& phases)
{
    info(ctx, "Blocks to flash: 0,  (total: 1)");
    info(ctx, "--- Erasing ECU flash memory ---");
    PhaseReporter erase = phases.start("Erase", 1);
    if (const Status erased = erase_memory(ctx, block); !erased.has_value())
    {
        return erased;
    }
    erase.complete();

    info(ctx, "--- Start writing ROM file to ECU flash memory ---");
    PhaseReporter write = phases.start("Write ROM", static_cast<int>(block.length));
    if (const Status written = reflash_block(ctx, image, block, write); !written.has_value())
    {
        return written;
    }
    write.complete();
    info(ctx, "Block 0 reflash complete.");
    return {};
}

} // namespace

Result<FlashExecutionResult> SubaruDensoSh72543CanDieselExecutor::execute(const FlashPlan& plan,
                                                                          IFlashTransport& transport, IClock& clock,
                                                                          const ICancellationToken& cancellation,
                                                                          IEventSink& events)
{
    if (const Status matched =
            check_family_transport_match(plan, FlashFamily::SubaruDensoSh72543CanDiesel, TransportKind::CanIso15765);
        !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = validate_subaru_denso_sh72543_can_diesel_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruDensoSh72543CanDieselPlan>(plan.family_plan());
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
    PhaseReporter connect = phases.start("Connect", 1);

    CanFlashUdsChannel channel(*can, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Ctx ctx{cancellation, events, clock, uds_client, channel};

    info(ctx, "Connecting to ECU Denso SH72543 Diesel CAN bootloader, please wait...");
    if (const Status connected = connect_bootloader(ctx, *can); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connect.complete();

    if (read)
    {
        events.notice("Reading ROM, please wait...");
        info(ctx, "Reading ROM from ECU, Denso SH72543 Diesel using CAN");

        PhaseReporter read_phase = phases.start("Read ROM", static_cast<int>(plan.transfer_region().length));
        Result<bytes::Bytes> rom = read_memory(ctx, family, plan.transfer_region(), read_phase);
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        read_phase.complete();
        return FlashExecutionResult{
            .operation = FlashOperation::Read,
            .read_bytes = std::move(*rom),
        };
    }

    // build_subaru_denso_sh72543_can_diesel_plan refuses TestWrite; the guard
    // is repeated here so a plan built another way cannot turn a dry run into
    // a real erase and write. Legacy threaded test_write all the way down to
    // reflash_block and then never consulted it.
    if (plan.operation() != FlashOperation::Write)
    {
        return fail(ErrorKind::Unsupported,
                    "test_write is not supported by the Subaru Denso SH72543 Diesel CAN family");
    }

    events.notice("Writing ROM, please wait...");
    info(ctx, "Writing ROM to ECU, Denso SH72543 Diesel using CAN");
    if (const Status written = write_memory(ctx, *plan.image(), plan.transfer_region(), phases); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{
        .operation = plan.operation(),
        .read_bytes = std::nullopt,
    };
}

} // namespace fastecu::flash
