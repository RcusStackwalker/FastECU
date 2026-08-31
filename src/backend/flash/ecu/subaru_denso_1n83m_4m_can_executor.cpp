#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_1n83m_4m_can_operation.cpp
// it was transcribed from.
//
// This family is the tolerant member of the wave-4 Denso ISO-15765 cluster:
// seven of its response checks have their `return STATUS_ERROR` commented out
// (lines 305, 335, 369, 876, 883, 917, 924), so it logs and carries on where
// its 1N83M 1.5M and SH72531 siblings abort. It also reads twice after the
// bench kernel jump where they read once, and spells its read timeouts as
// bare 200/500 literals rather than through the header's named constants.
// All of that is transcribed as found; each tolerated point is marked below.
namespace fastecu::flash
{
namespace
{
using namespace bytes::literals;
using bytes::composeBe;

// Legacy's read timeouts. This family writes most of them as bare literals
// (200 at lines 677, 709, 754, 788, 790; 500 at lines 1056, 1409, 1452) where
// the header still declares serial_read_short_timeout = 200 and
// receive_timeout = 500 (header lines 43-49); the numbers, not the spellings,
// are the wire behaviour, and they are kept apart rather than flattened into
// one policy.
constexpr int kShortTimeoutMs = 200;   // serial_read_short_timeout, and the bare 200 literals
constexpr int kReceiveTimeoutMs = 500; // receive_timeout, and the bare 500 literals
constexpr int kLongTimeoutMs = 2000;   // serial_read_timeout, and read_memory's bare 2000 at line 967
constexpr uds::ExchangePolicy kShortPolicy{.read_timeout_ms = kShortTimeoutMs};
constexpr uds::ExchangePolicy kReceivePolicy{.read_timeout_ms = kReceiveTimeoutMs};
constexpr uds::ExchangePolicy kLongPolicy{.read_timeout_ms = kLongTimeoutMs};
constexpr int kExtraLongTimeoutMs = 3000; // serial_read_extra_long_timeout
// Checksum verify reads twice and this family's two reads differ: the first
// uses the bare 500 literal (line 1322, i.e. receive_timeout) and the re-read
// after the ECU's 7F 31 78 pending answer uses
// serial_read_extra_long_timeout (line 1341). UdsClient substitutes
// pending_timeout_ms for that second read, so the pair is spelled out here
// rather than left to the 3000 ms default -- the three sibling families read
// 500/500, 2000/2000 and 2000/2000 at this same exchange, so the default is
// not a shared number and each family pins its own.
constexpr uds::ExchangePolicy kChecksumPolicy{.read_timeout_ms = kReceiveTimeoutMs,
                                              .pending_timeout_ms = kExtraLongTimeoutMs};

// Session ids in ISO 14229-1's 0x40-0x5F vehicle-manufacturer-specific band;
// legacy uses its own values here rather than the standard subfunctions.
constexpr bytes::Byte kSessionProbe = 0x5F;     // OBK probe / access-method probe
constexpr bytes::Byte kSessionInCarOpen = 0x63; // in-car "session open" subfunction
constexpr bytes::Byte kSessionBench = 0x43;     // bench programming session
constexpr bytes::Byte kSessionBenchJump = 0x42; // bench jump to on-board kernel
constexpr bytes::Byte kSessionInCarJump = 0x62; // in-car jump to on-board kernel
// Sent to 0x7A2 only (line 385); legacy names it by its value alone, and
// nothing in the source says what the addressed module does with it.
constexpr bytes::Byte kSessionVendorC0 = 0xC0;

// SecurityAccess subfunctions: ISO 14229-1 pairs an odd requestSeed with the
// next even sendKey for the same level. This family uses level 0x61/0x62, not
// the 0x01/0x02 level uds_service_ids.h names.
constexpr bytes::Byte kSecurityAccessRequestSeed = 0x61;
constexpr bytes::Byte kSecurityAccessSendKey = 0x62;

// Positive-response service ids for the two SIDs this file inspects without
// going through UdsClient (which does the SID + 0x40 arithmetic itself).
constexpr bytes::Byte kSessionControlReply = uds::kSidDiagnosticSessionControl + 0x40;   // 0x50
constexpr bytes::Byte kReadDataByIdentifierReply = uds::kSidReadDataByIdentifier + 0x40; // 0x62
constexpr bytes::Byte kRequestDownloadReply = uds::kSidRequestDownload + 0x40;           // 0x74
constexpr bytes::Byte kRequestUploadReply = uds::kSidRequestUpload + 0x40;               // 0x75
constexpr bytes::Byte kRoutineControlReply = uds::kSidRoutineControl + 0x40;             // 0x71

// ISO 14229-1 services used only by the in-car arm's fire-and-forget run
// (legacy lines 379-498). Kept local per uds_service_ids.h's rule that a
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

// fblocks_N83M_4MB[0].start -- reflash_block indexes the image as
// newdata[i + blockaddr - fdt->fblocks[0].start] (line 1241) and write_memory
// hands it &data_array[0] (line 1153), the whole encrypted ROM, so byte 0 of
// the plan image is this address.
constexpr std::uint32_t kImageStart = 0x08F9C000;
constexpr std::uint32_t kPageSize = 0x100;

// Additional CAN request ids the in-car arm addresses (legacy lines 379-498).
// 0x7DF is ISO 15765-4's functional-broadcast id; the other three are
// physical ids of other modules, which legacy names only by their numbers.
constexpr std::uint32_t kInCarIdA2 = 0x7a2;
constexpr std::uint32_t kInCarIdFunctional = 0x7df;
constexpr std::uint32_t kInCarIdE1 = 0x7e1;
constexpr std::uint32_t kInCarIdB0 = 0x7b0;

bytes::Bytes seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kDensoIso15765SeedKeyTable, kDensoIso15765IndexTransformation);
}

bytes::Bytes encrypt_rom(bytes::ByteView image)
{
    return SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()), kDensoIso15765EncryptTable,
                                         kDensoIso15765IndexTransformation);
}

// Legacy decrypts the whole accumulated dump in one call (line 1072);
// SsmProtocol::calculatePayload transforms independent 4-byte words, so
// decrypting each 256-byte page as it arrives produces byte-identical output
// without a second full-ROM buffer.
bytes::Bytes decrypt_page(bytes::ByteView page)
{
    return SsmProtocol::calculatePayload(page, static_cast<std::uint32_t>(page.size()), kDensoIso15765DecryptTable,
                                         kDensoIso15765IndexTransformation);
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
// 131-281): each is info-logged on a matching reply and error-logged
// otherwise, but the connect sequence never halts here -- even a genuine
// exchange failure is logged and swallowed, mirroring legacy's total absence
// of early returns in that block.
void non_fatal_query(Ctx& ctx, bytes::ByteView pdu, std::optional<bytes::Byte> expected_subfunction,
                     std::string_view label)
{
    ::fastecu::flash::non_fatal_query(exchange_context(ctx, kLongPolicy), pdu, expected_subfunction, kRejectionPrefix,
                                      label);
}

// Legacy's "log a mismatch, abort only on an absent or too-short reply"
// probe shape (lines 283-311, 313-341, 347-377). fatal_query aborts on a
// mismatch and non_fatal_query never aborts, so neither fits; this goes
// through the channel directly and returns the envelope-stripped frame so a
// caller can read further bytes out of it.
//
// TOLERATED, lines 305 / 335 / 369: at all three of this shape's call sites
// the `return STATUS_ERROR` that would follow the mismatch log is commented
// out in legacy, so the early return is intentionally absent here too and
// the sequence continues with whatever the ECU said. (Its 1N83M 1.5M sibling
// has no return at these points either, so the behaviour matches there; the
// commented-out lines are this family's own record that the strictness was
// removed on purpose.) The absent-reply branch below still aborts: lines
// 310, 340 and 374 keep their live `return STATUS_ERROR`.
Result<bytes::Bytes> tolerant_probe(Ctx& ctx, bytes::ByteView pdu, bytes::Byte expected_service,
                                    bytes::Byte expected_subfunction, std::string_view subject)
{
    if (const Status sent = ctx.channel.send(pdu, ctx.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    Result<std::optional<bytes::Bytes>> received = ctx.channel.receive(kShortTimeoutMs, ctx.cancellation);
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

// read_memory's two dump-setup exchanges (lines 843-884 and 886-925). Unlike
// tolerant_probe this tolerates BOTH halves of the check.
//
// TOLERATED, lines 876 / 883 (the 0x34 RequestDownload setup) and 917 / 924
// (the 0x35 RequestUpload setup): all four `return STATUS_ERROR` statements
// are commented out, so neither a wrong reply nor an absent one stops the
// dump -- the early return is intentionally absent. This is the family's one
// genuine behavioural divergence from its 1N83M 1.5M and SH72531 siblings,
// which abort at all four. A transport-level failure is still propagated:
// legacy's read_serial_data has no error channel at all, so a broken bus has
// no legacy behaviour to preserve and must surface rather than be mistaken
// for an empty reply.
Status tolerant_setup(Ctx& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix)
{
    if (const Status sent = ctx.channel.send(pdu, ctx.cancellation); !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> received = ctx.channel.receive(kShortTimeoutMs, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    // Legacy requires received.length() > 7, i.e. four bytes past the 4-byte
    // envelope, before it inspects the reply at all.
    if (!received->has_value() || received->value().size() < 4)
    {
        error(ctx, "No valid response from ECU");
        return {};
    }
    const bytes::Bytes& frame = **received;
    if (!std::equal(expected_prefix.begin(), expected_prefix.end(), frame.begin()))
    {
        error(ctx, std::format("{}{}", kRejectionPrefix, bytes::toHex(frame)));
    }
    return {};
}

// Legacy's in-car fire-and-forget exchange (lines 379-498): write on `id`,
// read whatever arrives next, discard it. Legacy validates neither the
// reply's arbitration id nor its content, so the reply is taken from the
// transport directly -- a CanFlashUdsChannel bound to `id` would impose a
// reply-id check legacy does not have. The channel is still used for the
// write, which is exactly its 4-byte-envelope job.
Status fire_and_forget(Ctx& ctx, ICanFlashTransport& can, std::uint32_t request_id, bytes::ByteView pdu)
{
    // The reply is read from `can` below, never through this channel, so the
    // response-id slot is filled with the request id and never consulted.
    CanFlashUdsChannel channel(can, request_id, request_id);
    if (const Status sent = channel.send(pdu, ctx.cancellation); !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> ignored = can.read(kShortTimeoutMs, ctx.cancellation);
    if (!ignored.has_value())
    {
        return std::unexpected(ignored.error());
    }
    return {};
}

// The seed request / seed key pair both arms share (bench lines 697-772,
// in-car lines 500-568). Both are fatal on a mismatch and on an absent
// reply -- neither arm's `return STATUS_ERROR` is commented out here.
Status security_access(Ctx& ctx)
{
    info(ctx, "Starting seed request");
    Result<bytes::Bytes> seed_reply =
        fatal_query(ctx, bytes::Bytes{uds::kSidSecurityAccess, kSecurityAccessRequestSeed},
                    bytes::Bytes{kSecurityAccessRequestSeed}, kShortPolicy, "seed request", 5);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    info(ctx, "Seed request ok");
    // Legacy reads the four seed bytes at raw frame offsets 6-9, i.e. payload
    // offsets 1-4 once the 4-byte envelope and the service id are stripped
    // (lines 531-534, 731-734).
    const bytes::Bytes key = seed_key(uds::payload(*seed_reply).subspan(1, 4));

    info(ctx, "Sending seed key");
    Result<bytes::Bytes> key_reply = fatal_query(ctx, composeBe(uds::kSidSecurityAccess, kSecurityAccessSendKey, key),
                                                 bytes::Bytes{kSecurityAccessSendKey}, kShortPolicy, "seed key");
    if (!key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    info(ctx, "Seed key ok");
    return {};
}

// Legacy's kernel jump plus its bounded re-read loop (bench lines 776-806,
// try_count < 50; in-car lines 631-659, try_count < 10). The loop's
// `init_ready` flag is never read after the loop and both arms fall straight
// through to `return STATUS_SUCCESS` (line 808), so an unacknowledged jump is
// logged here but is not an error -- deliberately not "fixed".
//
// `duplicate_pre_loop_read` reproduces the bench arm's extra read: legacy
// reads once at line 788, discards it, then reads again at line 790 and only
// that second frame seeds the wait loop. The in-car arm reads once (line
// 642), and neither the 1N83M 1.5M nor the SH72531 family reads twice at all.
Status jump_to_kernel(Ctx& ctx, bytes::Byte session, int max_tries, bool duplicate_pre_loop_read)
{
    info(ctx, "Jump to onboad kernel");
    if (const Status sent =
            ctx.channel.send(bytes::Bytes{uds::kSidDiagnosticSessionControl, session}, ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> received = ctx.channel.receive(kShortTimeoutMs, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (duplicate_pre_loop_read)
    {
        // Line 790's delay(50), then line 791's second read, whose result
        // overwrites the first and is the only one the loop ever sees.
        if (const Status slept = ctx.clock.sleep(50, ctx.cancellation); !slept.has_value())
        {
            return slept;
        }
        received = ctx.channel.receive(kShortTimeoutMs, ctx.cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
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
        received = ctx.channel.receive(kShortTimeoutMs, ctx.cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
    }
    error(ctx, "Kernel jump was not acknowledged; continuing, as legacy does");
    return {};
}

// In-car programming arm, legacy lines 345-661.
Status connect_in_car(Ctx& ctx, ICanFlashTransport& can)
{
    info(ctx, "In car programming: accessing, please wait...");
    if (const Status slept = ctx.clock.sleep(500, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    // Lines 347-377: mismatch logs and continues (TOLERATED, line 369), an
    // absent reply aborts.
    if (Result<bytes::Bytes> probe = tolerant_probe(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe},
                                                    kSessionControlReply, 0x01, "in-car access-method probe");
        !probe.has_value())
    {
        return std::unexpected(probe.error());
    }

    // Lines 379-498: ten fire-and-forget writes across four extra CAN ids.
    // Every reply is read and discarded, so a wrong byte here is invisible
    // on the wire without the scripted test that pins it.
    const struct
    {
        std::uint32_t id;
        bytes::Bytes pdu;
    } fire_and_forget_run[] = {
        {kInCarIdA2, {uds::kSidDiagnosticSessionControl, kSessionVendorC0}},                        // lines 379-389
        {0x7e0, {uds::kSidDiagnosticSessionControl, kSessionInCarOpen}},                            // lines 391-401
        {kInCarIdFunctional, {uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic}}, // lines 403-413
        {kInCarIdE1, {uds::kSidDiagnosticSessionControl, kSessionInCarOpen}},                       // lines 415-425
        {kInCarIdB0, {uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic}},         // lines 427-437
        {kInCarIdB0, {kSidControlDtcSetting, 0x02}},                                                // lines 439-449
        {kInCarIdFunctional, {kSidControlDtcSetting, 0x02}},                                        // lines 451-461
        {kInCarIdB0, {kSidControlDtcSetting, 0x02}},                                                // lines 463-473
        {kInCarIdFunctional, {kSidControlDtcSetting, 0x02}},                                        // lines 475-485
        {kInCarIdFunctional, {kSidCommunicationControl, 0x03, 0x01}},                               // lines 487-498
    };
    for (const auto& exchange : fire_and_forget_run)
    {
        if (const Status sent = fire_and_forget(ctx, can, exchange.id, exchange.pdu); !sent.has_value())
        {
            return sent;
        }
    }

    // Lines 500-568: seed/key on the primary 0x7E0 pair, fatal throughout.
    if (const Status unlocked = security_access(ctx); !unlocked.has_value())
    {
        return unlocked;
    }

    // Lines 572-599: unlike the earlier 0x10 0x5F probes, this one is fatal
    // on a mismatch, and expects subfunction 0x63 rather than 0x01.
    if (Result<bytes::Bytes> session =
            fatal_query(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe},
                        bytes::Bytes{kSessionInCarOpen}, kShortPolicy, "in-car session confirmation");
        !session.has_value())
    {
        return std::unexpected(session.error());
    }

    // Lines 601-629: the branch selector is re-read, this time fatally, and
    // its content past the 0x62 0x10 header is not inspected.
    if (Result<bytes::Bytes> selector =
            fatal_query(ctx, bytes::Bytes{uds::kSidReadDataByIdentifier, 0x10, 0x1D}, bytes::Bytes{0x10}, kShortPolicy,
                        "in-car programming-mode confirmation");
        !selector.has_value())
    {
        return std::unexpected(selector.error());
    }

    return jump_to_kernel(ctx, kSessionInCarJump, 10, false);
}

// Bench programming arm, legacy lines 663-808.
Status connect_bench(Ctx& ctx)
{
    info(ctx, "Bench programming: accessing, please wait...");
    if (const Status slept = ctx.clock.sleep(500, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    // Lines 667-695.
    if (Result<bytes::Bytes> session =
            fatal_query(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionBench},
                        bytes::Bytes{kSessionBench}, kShortPolicy, "bench diagnostic session");
        !session.has_value())
    {
        return std::unexpected(session.error());
    }

    // Lines 697-772.
    if (const Status unlocked = security_access(ctx); !unlocked.has_value())
    {
        return unlocked;
    }

    return jump_to_kernel(ctx, kSessionBenchJump, 50, true);
}

// Legacy connect_bootloader, lines 89-810. The legacy `is_serial_port_open()`
// guard (lines 95-101) has no equivalent: execute() below opens the transport
// through the port and propagates that Status, so an unopened bus fails
// earlier and with a typed error.
Status connect_bootloader(Ctx& ctx, ICanFlashTransport& can)
{
    // OBK-already-active probe (lines 105-127). A match short-circuits the
    // whole sequence; a mismatch OR an empty read is a non-fatal miss that
    // falls through to initializing the ECU, so this goes through the channel
    // directly rather than through ctx.uds.
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
        info(ctx, "OBK is active");
        return {};
    }
    info(ctx, "OBK not active, initialising ECU...");

    // Lines 131-281: four non-fatal identity queries.
    info(ctx, "Requesting ECU ID");
    non_fatal_query(ctx, bytes::Bytes{uds::kSidEcuIdQuery}, std::nullopt, "ECU ID");
    info(ctx, "Requesting VIN");
    non_fatal_query(ctx, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidVin}, uds::kVehicleInfoPidVin,
                    "VIN");
    info(ctx, "Requesting CAL ID");
    non_fatal_query(ctx, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId},
                    uds::kVehicleInfoPidCalId, "CAL ID");
    info(ctx, "Requesting CVN");
    non_fatal_query(ctx, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCvn}, uds::kVehicleInfoPidCvn,
                    "CVN");

    // Lines 283-311: mismatch logs and continues (TOLERATED, line 305), an
    // absent reply aborts.
    info(ctx, "Checking access method");
    if (Result<bytes::Bytes> access =
            tolerant_probe(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe}, kSessionControlReply,
                           0x01, "access-method probe");
        !access.has_value())
    {
        return std::unexpected(access.error());
    }

    // Lines 313-341: the branch selector, same tolerant shape (TOLERATED,
    // line 335).
    Result<bytes::Bytes> selector = tolerant_probe(ctx, bytes::Bytes{uds::kSidReadDataByIdentifier, 0x10, 0x1D},
                                                   kReadDataByIdentifierReply, 0x10, "programming-branch selector");
    if (!selector.has_value())
    {
        return std::unexpected(selector.error());
    }
    // Line 343 reads raw frame byte 7, i.e. payload byte 3 once the envelope
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

// The 0x34 RequestDownload / 0x35 RequestUpload setup PDU legacy spells out
// as literals for this family (lines 845-860, 886-901, 1390-1405): the
// region's start and length, big-endian, behind the two format bytes.
bytes::Bytes setup_pdu(bytes::Byte service, const MemoryRegion& region)
{
    return composeBe(service, kDataFormatIdentifier, kAddressAndLengthFormat, region.start, region.length);
}

// Legacy read_memory, lines 821-1081. start_addr and length are overwritten
// with 0x08FAC000 / 0x003D3F00 at lines 834-836 regardless of the caller's
// arguments; the plan carries exactly that region, so `region` reproduces it.
Result<bytes::Bytes> read_memory(Ctx& ctx, const SubaruDenso1n83m_4mCanPlan& family, const MemoryRegion& region,
                                 PhaseReporter& progress)
{
    // Lines 843-925: unlike every sibling family, BOTH setup exchanges
    // tolerate a wrong reply AND an absent one -- see tolerant_setup for the
    // four commented-out returns this preserves.
    info(ctx, "Settting dump start & length");
    if (const Status download = tolerant_setup(ctx, setup_pdu(uds::kSidRequestDownload, region),
                                               bytes::Bytes{kRequestDownloadReply, 0x20, 0x01, 0x05});
        !download.has_value())
    {
        return std::unexpected(download.error());
    }
    if (const Status upload = tolerant_setup(ctx, setup_pdu(uds::kSidRequestUpload, region),
                                             bytes::Bytes{kRequestUploadReply, 0x20, 0x01, 0x01});
        !upload.has_value())
    {
        return std::unexpected(upload.error());
    }

    info(ctx, "Start reading ROM, please wait...");
    bytes::Bytes rom;
    rom.reserve(region.length);
    for (std::uint32_t offset = 0; offset < region.length; offset += kPageSize)
    {
        // Legacy stopRequested() at line 942, top of loop.
        if (ctx.cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "read cancelled");
        }
        const std::uint32_t addr = region.start + offset;
        // Lines 929-965: SID 0xB7 plus a 4-byte big-endian address; the reply
        // is 0xF7 plus one 256-byte encrypted page (lines 967-991). This
        // exchange keeps its live `return STATUS_ERROR` (lines 977, 984).
        Result<bytes::Bytes> chunk = fatal_request(ctx, composeBe(uds::kSidReadMemoryChunk, addr), kLongPolicy,
                                                   std::format("the flash read at 0x{:x}", addr));
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        const bytes::Bytes decrypted = decrypt_page(uds::payload(*chunk));
        rom.insert(rom.end(), decrypted.begin(), decrypted.end());
        progress.update(static_cast<int>(offset + kPageSize));
    }

    info(ctx, "ROM read complete");

    // Stop command (lines 1040-1070): up to six attempts, each re-sending
    // 0x37, and the loop's outcome is never checked -- legacy proceeds to
    // build the image whether or not the ECU ever answered 0x77. Legacy logs
    // only an empty continuation line on success here (line 1066), so unlike
    // its 1N83M 1.5M sibling there is no response line to reproduce.
    info(ctx, "Sending stop command");
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        if (Result<bytes::Bytes> stop =
                ctx.uds.request(bytes::Bytes{uds::kSidRequestTransferExit}, kReceivePolicy, ctx.cancellation);
            stop.has_value())
        {
            break;
        }
    }

    // Lines 1072-1078: the unread region below 0x08FAC000 is synthesized as
    // 0xFF at offset 0 and a 0x100 0xFF tail is added, so byte 0 of the
    // returned image is address kImageStart. Legacy spells the tail as
    // insert(0x3E3F00, ...) rather than append; 0x3E3F00 is exactly
    // 0x10000 + 0x3D3F00, the length of the buffer at that point, so the two
    // are the same operation.
    bytes::Bytes image;
    image.reserve(family.lead_pad_len + rom.size() + family.tail_pad_len);
    image.assign(family.lead_pad_len, 0xFF);
    image.insert(image.end(), rom.begin(), rom.end());
    image.insert(image.end(), family.tail_pad_len, 0xFF);
    return image;
}

// Legacy erase_memory, lines 1374-1481: the setup PDU, the erase trigger,
// then a bounded re-read loop that never re-sends. Both of this function's
// response checks keep their live `return STATUS_ERROR` (lines 1421, 1427):
// the tolerance is confined to read_memory's own setup pair.
Status erase_memory(Ctx& ctx, const MemoryRegion& region)
{
    info(ctx, "Setting flash start & length");
    if (Result<bytes::Bytes> setup =
            fatal_query(ctx, setup_pdu(uds::kSidRequestDownload, region), bytes::Bytes{0x20, 0x01, 0x05},
                        kReceivePolicy, "flash start & length setup");
        !setup.has_value())
    {
        return std::unexpected(setup.error());
    }

    info(ctx, "Erasing ECU ROM");
    // Lines 1432-1445. Sent through the channel rather than UdsClient: legacy
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

// Legacy reflash_block, lines 1180-1367, called for block 1 only.
Status reflash_block(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseReporter& progress)
{
    constexpr std::uint32_t kChunkSize = 256;
    const std::uint32_t max_chunks = block.length / kChunkSize;

    // The whole image is encrypted once up front (legacy write_memory line
    // 1109), not per chunk.
    const bytes::Bytes encrypted = encrypt_rom(image);

    info(ctx, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    for (std::uint32_t chunk_index = 0; chunk_index < max_chunks; ++chunk_index)
    {
        // Legacy's stopRequested() check (line 1206) returns 0 -- success --
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
        // Lines 1211-1242: SID 0xB6, a 4-byte big-endian address, and the 256
        // encrypted bytes at newdata[i + blockaddr - fblocks[0].start], where
        // newdata is write_memory's &data_array[0] (line 1153) -- the whole
        // encrypted image based at fblocks[0].start. Composed, the index is
        // the absolute flash address minus kImageStart.
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

    // Close-block retry loop (lines 1269-1303): up to six attempts, tolerant
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
            // Legacy appends the reply's hex to this line (line 1299);
            // the port had dropped it. Restored by the wave-4 cluster-factoring
            // pass. The hex is the envelope-stripped PDU where legacy's was the
            // raw frame, envelope included -- the same divergence the other
            // three cluster members' "Stop request response" lines carry.
            info(ctx, std::format("Closed succesfully: {}", bytes::toHex(*closed)));
            break;
        }
    }

    // Line 1305: a settle before the checksum write, which no read timeout
    // subsumes.
    if (const Status slept = ctx.clock.sleep(100, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    // Checksum verify (lines 1307-1362). The ECU answers pending
    // (0x7F 0x31 0x78) before the real result; UdsClient absorbs that NRC by
    // re-reading internally, so this is a single request() call. Legacy
    // additionally *required* the pending answer to arrive first and failed
    // without it; UdsClient accepts an immediate positive answer too.
    //
    // Both of legacy's reads here are reproduced at their own timeouts, and
    // they differ: the first uses the bare 500 literal (line 1322, i.e.
    // receive_timeout) and only the re-read after the pending NRC uses
    // serial_read_extra_long_timeout = 3000 ms (line 1341). kChecksumPolicy
    // carries that 500/3000 pair. It is this family's own pair, not a shared
    // one: the 1N83M 1.5M reads 500/500 and the two SH families read
    // 2000/2000 at the same exchange.
    info(ctx, "Verifying checksum");
    if (Result<bytes::Bytes> checksum = fatal_query(
            ctx,
            bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, kRoutineIdHigh, kRoutineChecksum, 0x01},
            bytes::Bytes{uds::kRoutineControlStart, kRoutineIdHigh}, kChecksumPolicy, "checksum verify");
        !checksum.has_value())
    {
        return std::unexpected(checksum.error());
    }
    info(ctx, "Checksum verified");
    return {};
}

// Legacy write_memory, lines 1095-1173. block_modified is {0, 1, 0} over
// numblocks == 3, so exactly one block is erased and reflashed: fblocks[1],
// which is the plan's transfer region.
Status write_memory(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseSequence& phases)
{
    info(ctx, "Blocks to flash: 1,  (total: 1)");
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
    info(ctx, "Block 1 reflash complete.");
    return {};
}

} // namespace

Result<FlashExecutionResult> SubaruDenso1n83m_4mCanExecutor::execute(const FlashPlan& plan, IFlashTransport& transport,
                                                                     IClock& clock,
                                                                     const ICancellationToken& cancellation,
                                                                     IEventSink& events)
{
    if (const Status matched =
            check_family_transport_match(plan, FlashFamily::SubaruDenso1n83m_4mCan, TransportKind::CanIso15765);
        !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = validate_subaru_denso_1n83m_4m_can_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruDenso1n83m_4mCanPlan>(plan.family_plan());
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

    info(ctx, "Connecting to ECU Denso 1N83M 4MB CAN bootloader, please wait...");
    if (const Status connected = connect_bootloader(ctx, *can); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connect.complete();

    if (read)
    {
        events.notice("Reading ROM, please wait...");
        info(ctx, "Reading ROM from ECU, Denso 1N83M 4MB using CAN");

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

    // build_subaru_denso_1n83m_4m_can_plan refuses TestWrite; the guard is
    // repeated here so a plan built another way cannot turn a dry run into a
    // real erase and write. Legacy threaded test_write all the way down to
    // reflash_block and then never consulted it.
    //
    // It cannot fire as the code stands: the plan validation at the top of
    // execute() rejects TestWrite before any I/O, and the Read branch has
    // already returned, so FlashOperation has no third value left to reach
    // here. The wave-4 cluster-factoring pass reviewed it across all four
    // families and kept it: it costs nothing at runtime and is the last
    // thing between a non-Write operation and a real erase-and-write of an
    // ECU should that entry validation ever be relaxed or the enum gain a
    // value. Do not delete it as "dead code".
    if (plan.operation() != FlashOperation::Write)
    {
        return fail(ErrorKind::Unsupported, "test_write is not supported by the Subaru Denso 1N83M 4M CAN family");
    }

    events.notice("Writing ROM, please wait...");
    info(ctx, "Writing ROM to ECU, Denso 1N83M 4MB using CAN");
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
