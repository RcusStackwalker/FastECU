#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>
#include <variant>

#include "src/algorithms/diagnostics/nrc_parser.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_mitsu_m32r_can_operation.cpp
// it was transcribed from. The legacy class calls the qt_colt.h "*Frame"
// shims (buildDiagnosticSessionFrame, buildChallengeSeedRequestFrame,
// keyToBytes, ...); those are QByteArray wrappers that delegate to the
// portable MitsuColtCan / MitsuColtCanVendorExt builders used here, so the
// bytes on the wire are identical.
namespace fastecu::flash
{
namespace
{

// Legacy field values, flash_ecu_mitsu_m32r_can_operation.h:56. (The sibling
// serial_read_extra_long_timeout, .h:57, is used only by the write path and
// arrives with it.)
constexpr int kReadTimeoutMs = 500;

// Offset of the service byte in every reply: the 4-byte CAN reply id
// precedes it. The legacy code indexes received.at(4) throughout.
constexpr std::size_t kServiceOffset = 4;

// UDS positive-response service id, spelled "(kServiceX + 0x40)" at every
// legacy comparison site.
constexpr bytes::Byte positive(bytes::Byte service)
{
    return static_cast<bytes::Byte>(service + 0x40);
}

struct Ctx
{
    ICanFlashTransport& transport;
    IClock& clock;
    const ICancellationToken& cancellation;
    IEventSink& events;
};

void info(Ctx& ctx, std::string_view message)
{
    ctx.events.log(LogLevel::Info, message);
}

void error(Ctx& ctx, std::string_view message)
{
    ctx.events.log(LogLevel::Error, message);
}

// Legacy build_request, flash_ecu_mitsu_m32r_can_operation.cpp:58-64: every
// request carries the 4-byte big-endian physical request id. Threaded from
// the plan rather than hardcoded to the legacy 0x7E0 literal, so a future
// target with a different request id cannot silently keep using this one.
bytes::Bytes build_request(std::uint32_t request_id, bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, request_id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// The legacy NRC context is always `received.mid(4, received.length() - 1)`
// -- and QByteArray::mid CLAMPS its length argument to what is available, so
// that expression yields the whole tail from the service byte to the end of
// the frame (received.length() - 4 bytes), NOT one byte less. Reproduced as
// a plain suffix; taking the literal arithmetic instead would drop the final
// byte and turn a minimal 3-byte negative response into "Not a valid answer".
std::string nrc_context(bytes::ByteView received)
{
    if (received.size() <= kServiceOffset)
    {
        return nrc_description({});
    }
    return nrc_description(received.subspan(kServiceOffset));
}

// One write / delay / read exchange. Replaces the legacy
// write_serial_data_echo_check + delay() + read_serial_data() triple.
Result<bytes::Bytes> exchange(Ctx& ctx, std::uint32_t request_id, bytes::ByteView payload,
                              int delay_ms, int timeout_ms)
{
    if (ctx.cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before request");
    }
    const bytes::Bytes out = build_request(request_id, payload);
    if (Status written = ctx.transport.write(out, ctx.cancellation); !written)
    {
        return std::unexpected(written.error());
    }
    if (Status slept = ctx.clock.sleep(delay_ms, ctx.cancellation); !slept)
    {
        return std::unexpected(slept.error());
    }
    Result<std::optional<bytes::Bytes>> received =
        ctx.transport.read(timeout_ms, ctx.cancellation);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        // Legacy read_serial_data() returns an empty QByteArray on timeout,
        // which then fails the caller's own length check. The portable seam
        // distinguishes "nothing arrived" from "a short frame arrived", so
        // that case is reported as Timeout rather than BadResponse.
        return fail(ErrorKind::Timeout, "no response within the read timeout");
    }
    return std::move(**received);
}

bool service_is(bytes::ByteView received, bytes::Byte service)
{
    return received.size() > kServiceOffset && received[kServiceOffset] == service;
}

// Legacy connect_bootloader, flash_ecu_mitsu_m32r_can_operation.cpp:66-168.
// The legacy `is_serial_port_open()` guard (lines 74-78) has no equivalent:
// execute() below opens the transport through the port and propagates that
// Status, so an unopened bus fails earlier and with a typed error instead of
// the legacy "ERROR: Serial port is not open." log line.
Status connect_bootloader(Ctx& ctx, const MitsuColtM32rCanPlan& family)
{
    using namespace MitsuColtCan;

    if (family.use_vendor_challenge)
    {
        // Lines 86-95.
        info(ctx, "Requesting vendor extension challenge seed...");
        Result<bytes::Bytes> received =
            exchange(ctx, family.request_id, MitsuColtCanVendorExt::buildChallengeSeedRequest(),
                     200, kReadTimeoutMs);
        if (!received)
        {
            return std::unexpected(received.error());
        }
        // Line 91: length > 10 and the three selector bytes must match.
        if (received->size() <= 10 ||
            !service_is(*received, positive(MitsuColtCanVendorExt::kServiceReadMemoryByAddress)) ||
            (*received)[5] != MitsuColtCanVendorExt::kVendorChallengeSelector ||
            (*received)[6] != MitsuColtCanVendorExt::kVendorChallengeSeedSubfunction)
        {
            error(ctx, std::format("Wrong vendor challenge response from ECU: {}",
                                   nrc_context(*received)));
            return fail(ErrorKind::BadResponse, "vendor challenge seed rejected");
        }

        // Lines 97-104: received.mid(7, 4).
        const bytes::ByteView seed_bytes = bytes::ByteView(*received).subspan(7, 4);
        info(ctx, std::format("Received vendor seed: {}", SsmProtocol::toHex(seed_bytes)));

        const std::uint32_t vendor_key = MitsuColtCanVendorExt::challengeInverseTransform(
            MitsuColtCanVendorExt::bytesToSeed(seed_bytes));
        const bytes::Bytes key_bytes = MitsuColtCanVendorExt::keyBytes(vendor_key);
        info(ctx, std::format("Calculated vendor key: {}", SsmProtocol::toHex(key_bytes)));

        // Lines 106-116.
        info(ctx, "Sending vendor key to ECU...");
        received = exchange(ctx, family.request_id,
                            MitsuColtCanVendorExt::buildChallengeKey(vendor_key), 200,
                            kReadTimeoutMs);
        if (!received)
        {
            return std::unexpected(received.error());
        }
        if (received->size() <= 6 ||
            !service_is(*received, positive(MitsuColtCanVendorExt::kServiceReadMemoryByAddress)) ||
            (*received)[5] != MitsuColtCanVendorExt::kVendorChallengeSelector ||
            (*received)[6] != MitsuColtCanVendorExt::kVendorChallengeKeySubfunction)
        {
            error(ctx,
                  std::format("Vendor challenge key rejected: {}", nrc_context(*received)));
            return fail(ErrorKind::BadResponse, "vendor challenge key rejected");
        }
        info(ctx, "Vendor challenge accepted");
    }

    // Lines 119-129. The session id comes from the plan; the builder picks
    // kSessionBasic for a Read and kSessionBootload for a Write, matching
    // legacy line 82's `needFactorySecurity ? kSessionBootload : kSessionBasic`.
    info(ctx, "Starting diagnostic session...");
    Result<bytes::Bytes> received = exchange(
        ctx, family.request_id, buildDiagnosticSession(family.session_id), 50, kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (received->size() <= 5 || !service_is(*received, positive(kServiceDiagnosticSession)) ||
        (*received)[5] != family.session_id)
    {
        error(ctx, std::format("Wrong response from ECU: {}", nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "diagnostic session rejected");
    }
    info(ctx, "Diagnostic session ok");

    // Lines 131-134: only the bootload session needs factory security access.
    if (family.session_id != kSessionBootload)
    {
        return {};
    }

    // Lines 136-145.
    info(ctx, "Requesting security seed...");
    received =
        exchange(ctx, family.request_id, buildSecurityAccessSeedRequest(), 200, kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (received->size() <= 9 || !service_is(*received, positive(kServiceSecurityAccess)) ||
        (*received)[5] != 5)
    {
        error(ctx, std::format("Wrong response from ECU: {}", nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "security seed rejected");
    }

    // Lines 147-153: received.mid(6, 4).
    const bytes::ByteView seed = bytes::ByteView(*received).subspan(6, 4);
    info(ctx, std::format("Received seed: {}", SsmProtocol::toHex(seed)));

    const bytes::Bytes key = seedKey(seed);
    info(ctx, std::format("Calculated seed key: {}", SsmProtocol::toHex(key)));

    // Lines 155-165.
    info(ctx, "Sending seed key to ECU...");
    received = exchange(ctx, family.request_id, buildSecurityAccessKey(key), 200, kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (received->size() <= 5 || !service_is(*received, positive(kServiceSecurityAccess)) ||
        (*received)[5] != 6)
    {
        error(ctx, std::format("Wrong response from ECU: {}", nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "security key rejected");
    }
    info(ctx, "Security access ok");

    return {};
}

// Legacy readFlashRange, flash_ecu_mitsu_m32r_can_operation.cpp:170-211.
// Progress is reported as (bytes done, bytes total) rather than the legacy
// integer percentage; the dialog converts. This preserves the emission
// points exactly -- one per chunk, after the chunk is appended.
Result<bytes::Bytes> read_flash_range(Ctx& ctx, const MitsuColtM32rCanPlan& family,
                                      std::uint32_t start_addr, std::uint32_t length)
{
    using namespace MitsuColtCan;

    bytes::Bytes data;
    data.reserve(length);
    const std::uint32_t end_addr = start_addr + length;

    for (std::uint32_t addr = start_addr; addr < end_addr;)
    {
        // Line 183: stopRequested() is polled only at the top of each chunk.
        if (ctx.cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "read cancelled");
        }

        // Lines 188-189.
        const std::uint32_t remaining = end_addr - addr;
        const auto chunk_len = static_cast<bytes::Byte>(
            remaining < kFlashReadBlockSize ? remaining : kFlashReadBlockSize);

        // Lines 191-194.
        Result<bytes::Bytes> received =
            exchange(ctx, family.request_id, buildReadMemoryByAddress(addr, chunk_len), 50,
                     kReadTimeoutMs);
        if (!received)
        {
            return std::unexpected(received.error());
        }
        // Line 196: length must cover header + service + payload.
        if (received->size() < kServiceOffset + 1u + chunk_len ||
            !service_is(*received, positive(kServiceReadMemoryByAddress)))
        {
            error(ctx, std::format("Wrong response from ECU at 0x{:x}: {}", addr,
                                   nrc_context(*received)));
            return fail(ErrorKind::BadResponse, "read chunk rejected");
        }

        // Lines 202-206: received.mid(5, chunkLen).
        data.insert(data.end(),
                    received->begin() + static_cast<std::ptrdiff_t>(kServiceOffset + 1),
                    received->begin() + static_cast<std::ptrdiff_t>(kServiceOffset + 1 + chunk_len));
        addr += chunk_len;

        ctx.events.progress(static_cast<int>(addr - start_addr), static_cast<int>(length));
    }

    return data;
}

} // namespace

Result<FlashExecutionResult> MitsuColtM32rCanExecutor::execute(
    const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events)
{
    if (Status matched = check_family_transport_match(plan, FlashFamily::MitsuColtM32rCan,
                                                      TransportKind::CanIso15765);
        !matched)
    {
        return std::unexpected(matched.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }

    // Checked downcast, not static_cast -- same shape and same ErrorKind as
    // DensoSh705xEepromCanExecutor::execute.
    auto *can = dynamic_cast<ICanFlashTransport *>(&transport);
    if (can == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "transport does not implement ICanFlashTransport");
    }

    const auto& family = std::get<MitsuColtM32rCanPlan>(plan.family_plan());
    Ctx ctx{*can, clock, cancellation, events};

    // Legacy line 32-33: configureIso15765Can(serial, "500000", 0x7E0, 0x7E8)
    // then open_serial_port(). Legacy never closes the port, and neither does
    // this executor -- the desktop adapter owns the port lifetime.
    if (Status configured = can->configure(Iso15765Config{
            .bitrate = family.bitrate,
            .request_id = family.request_id,
            .response_id = family.response_id,
            .extended_id = family.extended_id,
        });
        !configured)
    {
        return std::unexpected(configured.error());
    }
    if (Status opened = can->open(); !opened)
    {
        return std::unexpected(opened.error());
    }

    // Legacy line 35.
    info(ctx, "Connecting to Mitsubishi Colt CZT M32R CAN bootloader, please wait...");
    if (Status connected = connect_bootloader(ctx, family); !connected)
    {
        return std::unexpected(connected.error());
    }

    if (plan.operation() == FlashOperation::Read)
    {
        // Legacy lines 42-45 and 213-228.
        events.notice("Reading ROM, please wait...");
        info(ctx, "Reading ROM from ECU using CAN");
        events.progress(0, static_cast<int>(plan.transfer_region().length));
        info(ctx, "Start reading ROM, please wait...");

        Result<bytes::Bytes> rom = read_flash_range(ctx, family, plan.transfer_region().start,
                                                    plan.transfer_region().length);
        if (!rom)
        {
            return std::unexpected(rom.error());
        }
        info(ctx, "ROM read complete");
        // Legacy line 227's trailing progressChanged(100) is not reproduced:
        // the final chunk's own emission (read_flash_range above) already
        // reports done == total, so repeating it would only duplicate the
        // last progress pair.
        return FlashExecutionResult{
            .operation = FlashOperation::Read,
            .read_bytes = std::move(*rom),
        };
    }

    // write_mem() and its helpers land with the write path; a Write plan is
    // refused rather than silently succeeding until then.
    return fail(ErrorKind::Unsupported,
                "the Mitsu Colt M32R CAN write path is not implemented yet");
}

} // namespace fastecu::flash
