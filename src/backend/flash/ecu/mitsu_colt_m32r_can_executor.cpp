#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <utility>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

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

constexpr uds::ExchangePolicy kRoutineExchangePolicy{.read_timeout_ms = 500};
constexpr uds::ExchangePolicy kSlowExchangePolicy{.read_timeout_ms = 3000};

// The two SecurityAccess levels this family uses, echoed back in the
// subfunction byte of each reply. Spelled as the bare 5 and 6 at the legacy
// comparison sites (flash_ecu_mitsu_m32r_can_operation.cpp:145 and 163) and
// by buildSecurityAccessSeedRequest / buildSecurityAccessKey.
constexpr bytes::Byte kSecurityAccessSeedLevel = 0x05;
constexpr bytes::Byte kSecurityAccessKeyLevel = 0x06;

// Every exchange goes through UdsClient over CanFlashUdsChannel, so the
// frames seen here start at the service id: the 4-byte CAN reply id is added
// and stripped by the channel, the positive-response echo is checked by the
// client, and NRC 0x78 is absorbed there too. What is left at each site below
// is the family-specific content check.
struct Ctx
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    uds::UdsClient& uds;
};

void info(Ctx& ctx, std::string_view message)
{
    ctx.events.log(LogLevel::Info, message);
}

void error(Ctx& ctx, std::string_view message)
{
    ctx.events.log(LogLevel::Error, message);
}

// The single reporting point for every failed exchange below, because two
// kinds of failure mean opposite things to whoever reads the log -- see
// uds_client_exchange_common.h's own report_exchange_failure for the
// rejection-vs-cancellation rationale (identical here; the erase-trigger
// power-cycling scenario that motivated it is this family's own). This
// thin wrapper exists only so call sites below can keep passing `ctx`
// instead of `ctx.events`.
Error report_exchange_failure(Ctx& ctx, const Error& failure,
                              std::string_view rejection_prefix, std::string_view operation)
{
    return ::fastecu::flash::report_exchange_failure(ctx.events, failure, rejection_prefix,
                                                     operation);
}

// The fatal_request + expected-response-prefix check every exchange in
// connect_bootloader below repeats. Unlike the other three
// UdsExchangeContext-based CAN families, this one has no single fixed
// exchange policy or rejection prefix -- the vendor challenge block above
// uses its own prefixes per exchange, and erase/write (not shown here) use
// kSlowExchangePolicy -- so `policy` is built into a fresh UdsExchangeContext
// per call instead of a shared one, and rejection_prefix stays an explicit
// parameter. See uds_client_exchange_common.h's fatal_query for what
// expected_prefix, subject, and min_payload_size mean.
Result<bytes::Bytes> fatal_query(Ctx& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix,
                                 const uds::ExchangePolicy& policy, std::string_view rejection_prefix,
                                 std::string_view subject,
                                 std::optional<std::size_t> min_payload_size = std::nullopt)
{
    return ::fastecu::flash::fatal_query(
        UdsExchangeContext{ctx.uds, policy, ctx.cancellation, ctx.events}, pdu, expected_prefix,
        rejection_prefix, subject, min_payload_size);
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
        info(ctx, "Starting basic diagnostic session for vendor authorization...");
        Result<bytes::Bytes> received = fatal_query(
            ctx, buildDiagnosticSession(kSessionBasic), bytes::Bytes{kSessionBasic},
            kRoutineExchangePolicy, "Wrong response from ECU: ", "basic diagnostic session");
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        info(ctx, "Basic diagnostic session ok");

        // Lines 86-95. The reply must carry the two selector bytes plus a
        // 4-byte seed after them (legacy "length > 10" on the enveloped
        // frame).
        info(ctx, "Requesting vendor extension challenge seed...");
        received = fatal_query(
            ctx, MitsuColtCanVendorExt::buildChallengeSeedRequest(),
            bytes::Bytes{MitsuColtCanVendorExt::kVendorChallengeSelector,
                         MitsuColtCanVendorExt::kVendorChallengeSeedSubfunction},
            kRoutineExchangePolicy, "Wrong vendor challenge response from ECU: ",
            "vendor challenge seed request", 6);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }

        // Lines 97-104: received.mid(7, 4) on the enveloped frame.
        const bytes::ByteView seed_bytes = uds::payload(*received).subspan(2, 4);
        info(ctx, std::format("Received vendor seed: {}", bytes::toHex(seed_bytes)));

        const std::uint32_t vendor_key = MitsuColtCanVendorExt::challengeInverseTransform(
            MitsuColtCanVendorExt::bytesToSeed(seed_bytes));
        const bytes::Bytes key_bytes = MitsuColtCanVendorExt::keyBytes(vendor_key);
        info(ctx, std::format("Calculated vendor key: {}", bytes::toHex(key_bytes)));

        // Lines 106-116. Echoing the selector is not acceptance: only
        // kVendorChallengeAccepted grants the transition, so this stays a
        // content check of its own.
        info(ctx, "Sending vendor key to ECU...");
        received = fatal_query(
            ctx, MitsuColtCanVendorExt::buildChallengeKey(vendor_key),
            bytes::Bytes{MitsuColtCanVendorExt::kVendorChallengeSelector,
                         MitsuColtCanVendorExt::kVendorChallengeAccepted},
            kRoutineExchangePolicy, "Vendor challenge key rejected: ", "vendor challenge key");
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        info(ctx, "Vendor challenge accepted");
    }

    // Vendor-extension plans enter the basic session above only long enough
    // to authorize the transition. All reads and writes then enter the
    // bootload session and complete factory SecurityAccess.
    info(ctx, "Starting diagnostic session...");
    Result<bytes::Bytes> received = fatal_query(
        ctx, buildDiagnosticSession(family.session_id), bytes::Bytes{family.session_id},
        kRoutineExchangePolicy, "Wrong response from ECU: ", "diagnostic session");
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    info(ctx, "Diagnostic session ok");

    // Lines 131-134: only the bootload session needs factory security access.
    if (family.session_id != kSessionBootload)
    {
        return {};
    }

    // Lines 136-145.
    // The seed level byte plus the 4 seed bytes behind it (legacy "length > 9"
    // on the enveloped frame).
    info(ctx, "Requesting security seed...");
    received = fatal_query(ctx, buildSecurityAccessSeedRequest(), bytes::Bytes{kSecurityAccessSeedLevel},
                           kRoutineExchangePolicy, "Wrong response from ECU: ",
                           "security access seed request", 5);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }

    // Lines 147-153: received.mid(6, 4) on the enveloped frame.
    const bytes::ByteView seed = uds::payload(*received).subspan(1, 4);
    info(ctx, std::format("Received seed: {}", bytes::toHex(seed)));

    const bytes::Bytes key = seedKey(seed);
    info(ctx, std::format("Calculated seed key: {}", bytes::toHex(key)));

    // Lines 155-165.
    info(ctx, "Sending seed key to ECU...");
    received = fatal_query(ctx, buildSecurityAccessKey(key), bytes::Bytes{kSecurityAccessKeyLevel},
                           kRoutineExchangePolicy, "Wrong response from ECU: ",
                           "security access key");
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    info(ctx, "Security access ok");

    return {};
}

// Legacy readFlashRange, flash_ecu_mitsu_m32r_can_operation.cpp:170-211.
// Progress is reported as (bytes done, bytes total) rather than the legacy
// integer percentage; the dialog converts. This preserves the emission
// points exactly -- one per chunk, after the chunk is appended.
Result<bytes::Bytes> read_flash_range(Ctx& ctx, std::uint32_t start_addr, std::uint32_t length,
                                      PhaseReporter *progress = nullptr)
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
            ctx.uds.request(buildReadMemoryByAddress(addr, chunk_len), kRoutineExchangePolicy,
                            ctx.cancellation);
        if (!received.has_value())
        {
            return std::unexpected(report_exchange_failure(
                ctx, received.error(), std::format("Wrong response from ECU at 0x{:x}: ", addr),
                std::format("the flash read at 0x{:x}", addr)));
        }
        // Line 196: the reply must carry the whole chunk it was asked for. A
        // short one is refused rather than padded -- silently accepting it
        // would leave a hole in the image the verify pass then blames on the
        // flash write.
        const bytes::ByteView payload = uds::payload(*received);
        if (payload.size() < chunk_len)
        {
            error(ctx, std::format("Wrong response from ECU at 0x{:x}: expected {} payload "
                                   "bytes, got {}",
                                   addr, static_cast<unsigned>(chunk_len), payload.size()));
            return fail(ErrorKind::BadResponse, "read chunk rejected");
        }

        // Lines 202-206: received.mid(5, chunkLen) on the enveloped frame.
        const bytes::ByteView chunk = payload.subspan(0, chunk_len);
        data.insert(data.end(), chunk.begin(), chunk.end());
        addr += chunk_len;

        if (progress != nullptr)
        {
            progress->update(static_cast<int>(addr - start_addr));
        }
    }

    return data;
}

// Legacy upload_and_commit, flash_ecu_mitsu_m32r_can_operation.cpp:231-297.
Status upload_and_commit(Ctx& ctx, std::uint32_t start, bytes::ByteView data,
                         PhaseReporter *progress = nullptr)
{
    using namespace MitsuColtCan;

    // Lines 238-246.
    Result<bytes::Bytes> received =
        ctx.uds.request(buildRequestDownload(start, static_cast<std::uint32_t>(data.size())),
                        kRoutineExchangePolicy, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(report_exchange_failure(
            ctx, received.error(), std::format("RequestDownload to 0x{:x} rejected: ", start),
            std::format("RequestDownload to 0x{:x}", start)));
    }

    // Lines 248-260.
    std::uint32_t payload_done = 0;
    for (const bytes::Bytes& chunk : buildTransferDataFrames(data))
    {
        received = ctx.uds.request(chunk, kRoutineExchangePolicy, ctx.cancellation);
        if (!received.has_value())
        {
            return std::unexpected(report_exchange_failure(
                ctx, received.error(), std::format("TransferData to 0x{:x} rejected: ", start),
                std::format("TransferData to 0x{:x}", start)));
        }
        payload_done += static_cast<std::uint32_t>(chunk.size() - 1);
        if (progress != nullptr)
        {
            progress->update(static_cast<int>(payload_done));
        }
    }

    // Lines 262-270.
    received = ctx.uds.request(buildRequestDownload(kCrcTransferAddress, kCrcTransferSize),
                               kRoutineExchangePolicy, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(report_exchange_failure(
            ctx, received.error(), "RequestDownload for checksum rejected: ",
            "RequestDownload for the checksum"));
    }

    // Lines 272-284: big-endian 16-bit running sum, always exactly one
    // TransferData frame (kCrcTransferSize is 2, well under kTransferChunkSize).
    const std::uint16_t crc = checksum(data);
    received = ctx.uds.request(buildTransferDataFrames(bytes::composeBe(crc)).front(),
                               kRoutineExchangePolicy, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(report_exchange_failure(
            ctx, received.error(), "TransferData for checksum rejected: ",
            "TransferData for the checksum"));
    }

    // Lines 286-294: the CRC check gets the extra-long timeout.
    received = ctx.uds.request(buildRoutineCheckCrc(start), kSlowExchangePolicy,
                               ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(report_exchange_failure(
            ctx, received.error(),
            std::format("RoutineControl CRC check for 0x{:x} rejected: ", start),
            std::format("the RoutineControl CRC check for 0x{:x}", start)));
    }

    if (progress != nullptr)
    {
        progress->complete();
    }

    return {};
}

// Legacy: the unlock + erase-trigger pair that appears identically in
// ensureTopRegionWritten (lines 349-368) and write_mem (lines 445-464). The
// only difference between the two copies is which stage of the write they
// belong to, which the legacy code carried in the log-message text, so that
// is a parameter here rather than two transcriptions. `stage` is empty for
// the main write and " (top 128KB bootstrap)" for the bootstrap copy, giving
// back the two legacy prefixes exactly.
Status unlock_and_erase(Ctx& ctx, std::string_view stage)
{
    using namespace MitsuColtCan;

    Result<bytes::Bytes> received =
        ctx.uds.request(buildRequestReflashUnlock(), kSlowExchangePolicy, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(report_exchange_failure(
            ctx, received.error(), std::format("Reflash unlock{} rejected: ", stage),
            std::format("the reflash unlock request{}", stage)));
    }

    received = ctx.uds.request(buildRoutineErase(), kSlowExchangePolicy, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(report_exchange_failure(
            ctx, received.error(), std::format("Erase trigger{} rejected: ", stage),
            std::format("the erase trigger{}", stage)));
    }
    if (ctx.cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after erase");
    }

    return {};
}

// The portable replacement for the legacy mid-operation confirm() gates
// (lines 320-333 and 433-443). A dialog-free executor cannot block for a
// human answer, so each gate became a ConfirmationSpec the desktop dialog
// answers BEFORE execute() runs: presence in plan.confirmations() means
// granted, absence means the operator declined or was never asked. Checked
// rather than assumed -- validate_and_build does not require these specs, so
// a hand-built plan could otherwise reach the erase trigger ungated.
bool confirmation_granted(const FlashPlan& plan, ConfirmationSpec::Id id)
{
    return std::ranges::any_of(plan.confirmations(),
                               [id](const ConfirmationSpec& spec)
                               { return spec.id == id; });
}

// Legacy ensureTopRegionWritten, flash_ecu_mitsu_m32r_can_operation.cpp:299-390.
// This path applies only to the 512 KiB protocols; the 384 KiB protocols end
// exactly where the protected top region begins.
Status ensure_top_region_written(Ctx& ctx, const FlashPlan& plan, bytes::ByteView rom,
                                 PhaseSequence& phases)
{
    using namespace MitsuColtCan;

    PhaseReporter phase = phases.start("Ensure top region", 3);

    // Line 303.
    info(ctx, std::format("Checking top 128KB (0x{:x}-0x{:x})...", kTopRegionStart,
                          kTopRegionEnd));

    // Lines 305-309.
    Result<bytes::Bytes> current_top = read_flash_range(ctx, kTopRegionStart, kTopRegionLength);
    if (!current_top.has_value())
    {
        return std::unexpected(current_top.error());
    }

    // Lines 311-316. The legacy `romdata.mid(kTopRegionStart, kTopRegionLength)`
    // clamps a short slice; write_mem's length guard rules that out before it
    // calls here, so this subspan is always the full kTopRegionLength bytes.
    const bytes::ByteView wanted_top = rom.subspan(kTopRegionStart, kTopRegionLength);
    if (std::ranges::equal(*current_top, wanted_top))
    {
        info(ctx, "Top 128KB already matches, no bootstrap needed");
        phase.complete();
        return {};
    }
    phase.update(1);

    info(ctx, "Top 128KB mismatch, bootstrapping via redirect routines...");

    // Lines 320-333.
    if (!confirmation_granted(plan, ConfirmationSpec::Id::TopRegionBootstrap))
    {
        info(ctx, "Top 128KB bootstrap canceled by user");
        return fail(ErrorKind::Cancelled, "top region bootstrap was not confirmed");
    }

    // Lines 335-340.
    info(ctx, std::format("Uploading erase redirect routine to RAM 0x{:x}...",
                          kEraseRoutineRamAddr));
    if (const Status uploaded =
            upload_and_commit(ctx, kEraseRoutineRamAddr, kEraseRedirectRoutine);
        !uploaded.has_value())
    {
        error(ctx, "Erase redirect routine upload failed");
        return uploaded;
    }

    // Lines 342-347.
    info(ctx, std::format("Uploading write redirect routine to RAM 0x{:x}...",
                          kWriteRoutineRamAddr));
    if (const Status uploaded =
            upload_and_commit(ctx, kWriteRoutineRamAddr, kWriteRedirectRoutine);
        !uploaded.has_value())
    {
        error(ctx, "Write redirect routine upload failed");
        return uploaded;
    }

    // Lines 349-368.
    if (const Status erased = unlock_and_erase(ctx, " (top 128KB bootstrap)");
        !erased.has_value())
    {
        return erased;
    }
    info(ctx, "Carrier window erased");

    // Lines 370-375. The carrier address is kUserspaceStart, not
    // kTopRegionStart: the bootloader hard-validates RequestDownload targets
    // into the userspace window, and the redirect routines add the +0x058000
    // offset themselves. See mitsu_colt_can_protocol.h's kEraseRedirectRoutine
    // comment.
    if (const Status written = upload_and_commit(ctx, kUserspaceStart, wanted_top);
        !written.has_value())
    {
        error(ctx, "Top 128KB redirect write failed");
        return written;
    }
    info(ctx, "Top 128KB written via redirect");
    phase.update(2);

    // Lines 377-387.
    Result<bytes::Bytes> verify_top = read_flash_range(ctx, kTopRegionStart, kTopRegionLength);
    if (!verify_top.has_value())
    {
        return std::unexpected(verify_top.error());
    }
    if (!std::ranges::equal(*verify_top, wanted_top))
    {
        error(ctx, "Top 128KB verify failed after redirect write");
        return fail(ErrorKind::BadResponse, "top region verify mismatch");
    }
    info(ctx, "Top 128KB verified");
    phase.complete();

    return {};
}

// Legacy write_mem, flash_ecu_mitsu_m32r_can_operation.cpp:392-476.
Status write_mem(Ctx& ctx, const FlashPlan& plan, bytes::ByteView rom, PhaseSequence& phases)
{
    using namespace MitsuColtCan;

    const std::uint32_t writable_end =
        plan.transfer_region().start + plan.transfer_region().length;
    const bool includes_top_region = writable_end == MitsuColtCan::kFullRomSize;
    const std::uint32_t page_write_end =
        includes_top_region ? MitsuColtCan::kTopRegionStart : writable_end;

    // The legacy 512 KiB flow must compare and, if needed, bootstrap the top
    // 128 KiB. A 384 KiB image has no top region, so it proceeds directly to
    // the stock page helpers.
    if (includes_top_region)
    {
        if (const Status bootstrapped = ensure_top_region_written(ctx, plan, rom, phases);
            !bootstrapped.has_value())
        {
            return bootstrapped;
        }
    }

    PhaseReporter prepare = phases.start("Prepare userspace", 2);

    // Lines 409-415.
    info(ctx,
         std::format("Uploading erase-page routine to RAM 0x{:x}...", kEraseRoutineRamAddr));
    if (const Status uploaded = upload_and_commit(ctx, kEraseRoutineRamAddr, kErasePageRoutine);
        !uploaded.has_value())
    {
        error(ctx, "Erase-page routine upload failed");
        return uploaded;
    }
    info(ctx, "Erase page uploaded");
    prepare.update(1);

    // Lines 417-423.
    info(ctx,
         std::format("Uploading write-page routine to RAM 0x{:x}...", kWriteRoutineRamAddr));
    if (const Status uploaded = upload_and_commit(ctx, kWriteRoutineRamAddr, kWritePageRoutine);
        !uploaded.has_value())
    {
        error(ctx, "Write-page routine upload failed");
        return uploaded;
    }
    info(ctx, "Write page uploaded");
    prepare.complete();

    PhaseReporter erase = phases.start("Erase userspace", 1);

    // --- HIGH RISK STEP ---
    // The 12-byte ServiceRequestReflash(0x3B) payload unlock_and_erase sends
    // below is carried over verbatim from
    // externals/livemonitor/obdsessionwidget.cpp:180-181, where the original
    // author's own comment reads "caused bootloader lockup" during their
    // testing. Only attempt this on a bench/spare ECU with a recovery path
    // available (see this project's boot-talk utility for bricked-ECU
    // recovery). Legacy lines 425-443 gated it behind a QMessageBox; the
    // portable gate is the plan's EraseTrigger confirmation.
    if (!confirmation_granted(plan, ConfirmationSpec::Id::EraseTrigger))
    {
        info(ctx, "Erase trigger canceled by user");
        return fail(ErrorKind::Cancelled, "erase trigger was not confirmed");
    }

    // Lines 445-464.
    if (const Status erased = unlock_and_erase(ctx, ""); !erased.has_value())
    {
        return erased;
    }
    info(ctx, "Userspace flash erased");
    erase.complete();

    // Lines 466-473, extended to stop at the capacity snapshotted in the
    // selected protocol plan.
    info(ctx, std::format("Writing ROM userspace 0x{:x}-0x{:x}...", kUserspaceStart,
                          page_write_end));
    const bytes::ByteView userspace =
        rom.subspan(kUserspaceStart, page_write_end - kUserspaceStart);
    PhaseReporter write =
        phases.start("Write userspace", static_cast<int>(userspace.size()) + 1);
    if (const Status written = upload_and_commit(ctx, kUserspaceStart, userspace, &write);
        !written.has_value())
    {
        error(ctx, "ROM userspace write failed");
        return written;
    }
    info(ctx, "Userspace flash written");

    PhaseReporter verify =
        phases.start("Verify userspace", static_cast<int>(userspace.size()) + 1);
    Result<bytes::Bytes> verify_userspace =
        read_flash_range(ctx, kUserspaceStart, page_write_end - kUserspaceStart, &verify);
    if (!verify_userspace.has_value())
    {
        return std::unexpected(verify_userspace.error());
    }
    if (!std::ranges::equal(*verify_userspace, userspace))
    {
        error(ctx, "Userspace verify failed after write");
        return fail(ErrorKind::BadResponse, "userspace verify mismatch");
    }
    info(ctx, "Userspace flash verified");
    verify.complete();

    return {};
}

} // namespace

Result<FlashExecutionResult> MitsuColtM32rCanExecutor::execute(
    const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events)
{
    if (const Status matched = check_family_transport_match(plan, FlashFamily::MitsuColtM32rCan,
                                                            TransportKind::CanIso15765);
        !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = validate_mitsu_colt_m32r_can_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }

    const auto& family = std::get<MitsuColtM32rCanPlan>(plan.family_plan());
    // Checked downcast, not static_cast -- same shape and same ErrorKind as
    // DensoSh705xEepromCanExecutor::execute.
    auto *can = dynamic_cast<ICanFlashTransport *>(&transport);
    if (can == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "transport does not implement ICanFlashTransport");
    }

    const std::uint32_t rom_end =
        plan.transfer_region().start + plan.transfer_region().length;
    const bool read = plan.operation() == FlashOperation::Read;
    PhaseSequence phases(events, read ? 2 : (rom_end == MitsuColtCan::kFullRomSize ? 6 : 5));
    PhaseReporter connect = phases.start(read ? "Connect to ECU" : "Connect", 1);

    // Legacy line 32-33: configureIso15765Can(serial, "500000", 0x7E0, 0x7E8)
    // then open_serial_port(). Legacy never closes the port, and neither does
    // this executor -- the desktop adapter owns the port lifetime.
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

    // The channel owns the 4-byte CAN id envelope and the client owns the
    // exchange; both are stack-scoped here so they outlive every phase below.
    CanFlashUdsChannel channel(*can, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Ctx ctx{cancellation, events, uds_client};

    // Legacy line 35.
    info(ctx, "Connecting to Mitsubishi Colt CZT M32R CAN bootloader, please wait...");
    if (const Status connected = connect_bootloader(ctx, family); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connect.complete();

    if (plan.operation() == FlashOperation::Read)
    {
        // The protocol plan supplies a zero-based, capacity-sized range; the
        // chunk loop itself remains generic.
        events.notice("Reading ROM, please wait...");
        info(ctx, "Reading ROM from ECU using CAN");
        info(ctx, "Start reading ROM, please wait...");

        PhaseReporter read_phase =
            phases.start("Read ROM", static_cast<int>(plan.transfer_region().length));
        Result<bytes::Bytes> rom = read_flash_range(ctx, plan.transfer_region().start,
                                                    plan.transfer_region().length, &read_phase);
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        read_phase.complete();
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

    // Legacy execute() (lines 40-53) dispatched on cmd_type "read"/"write" and
    // silently reported success for anything else. TestWrite is refused by
    // build_mitsu_colt_m32r_can_plan; the guard is repeated here so a plan
    // built another way cannot turn a dry run into a real erase and write.
    if (plan.operation() != FlashOperation::Write)
    {
        return fail(ErrorKind::Unsupported,
                    "test_write is not supported by the Mitsu Colt M32R CAN family");
    }

    // Legacy lines 49-51.
    events.notice("Writing ROM, please wait...");
    info(ctx, "Writing ROM to ECU using CAN");
    // validate_and_build guarantees a Write plan carries an image; write_mem
    // re-checks its length, as the legacy write_mem did.
    if (const Status written = write_mem(ctx, plan, *plan.image(), phases);
        !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{
        .operation = plan.operation(),
        .read_bytes = std::nullopt,
    };
}

} // namespace fastecu::flash
