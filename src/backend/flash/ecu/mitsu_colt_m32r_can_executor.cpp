#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <utility>
#include <variant>

#include "src/algorithms/diagnostics/nrc_parser.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

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

// Legacy field values, flash_ecu_mitsu_m32r_can_operation.h:56-57.
constexpr int kReadTimeoutMs = 500;
constexpr int kExtraLongTimeoutMs = 3000;

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
        info(ctx, "Starting basic diagnostic session for vendor authorization...");
        Result<bytes::Bytes> received =
            exchange(ctx, family.request_id, buildDiagnosticSession(kSessionBasic), 50,
                     kReadTimeoutMs);
        if (!received)
        {
            return std::unexpected(received.error());
        }
        if (received->size() <= 5 ||
            !service_is(*received, positive(kServiceDiagnosticSession)) ||
            (*received)[5] != kSessionBasic)
        {
            error(ctx, std::format("Wrong response from ECU: {}", nrc_context(*received)));
            return fail(ErrorKind::BadResponse, "basic diagnostic session rejected");
        }
        info(ctx, "Basic diagnostic session ok");

        // Lines 86-95.
        info(ctx, "Requesting vendor extension challenge seed...");
        received = exchange(ctx, family.request_id,
                            MitsuColtCanVendorExt::buildChallengeSeedRequest(), 200,
                            kReadTimeoutMs);
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
        info(ctx, std::format("Received vendor seed: {}", bytes::toHex(seed_bytes)));

        const std::uint32_t vendor_key = MitsuColtCanVendorExt::challengeInverseTransform(
            MitsuColtCanVendorExt::bytesToSeed(seed_bytes));
        const bytes::Bytes key_bytes = MitsuColtCanVendorExt::keyBytes(vendor_key);
        info(ctx, std::format("Calculated vendor key: {}", bytes::toHex(key_bytes)));

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
            (*received)[6] != MitsuColtCanVendorExt::kVendorChallengeAccepted)
        {
            error(ctx,
                  std::format("Vendor challenge key rejected: {}", nrc_context(*received)));
            return fail(ErrorKind::BadResponse, "vendor challenge key rejected");
        }
        info(ctx, "Vendor challenge accepted");
    }

    // Vendor-extension plans enter the basic session above only long enough
    // to authorize the transition. All reads and writes then enter the
    // bootload session and complete factory SecurityAccess.
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
    info(ctx, std::format("Received seed: {}", bytes::toHex(seed)));

    const bytes::Bytes key = seedKey(seed);
    info(ctx, std::format("Calculated seed key: {}", bytes::toHex(key)));

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
                                      std::uint32_t start_addr, std::uint32_t length,
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

        if (progress != nullptr)
        {
            progress->update(static_cast<int>(addr - start_addr));
        }
    }

    return data;
}

// Legacy upload_and_commit, flash_ecu_mitsu_m32r_can_operation.cpp:231-297.
Status upload_and_commit(Ctx& ctx, const MitsuColtM32rCanPlan& family, std::uint32_t start,
                         bytes::ByteView data, PhaseReporter *progress = nullptr)
{
    using namespace MitsuColtCan;

    // Lines 238-246.
    Result<bytes::Bytes> received =
        exchange(ctx, family.request_id,
                 buildRequestDownload(start, static_cast<std::uint32_t>(data.size())), 50,
                 kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, positive(kServiceRequestDownload)))
    {
        error(ctx, std::format("RequestDownload to 0x{:x} rejected: {}", start,
                               nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "RequestDownload rejected");
    }

    // Lines 248-260.
    std::uint32_t payload_done = 0;
    for (const bytes::Bytes& chunk : buildTransferDataFrames(data))
    {
        received = exchange(ctx, family.request_id, chunk, 50, kReadTimeoutMs);
        if (!received)
        {
            return std::unexpected(received.error());
        }
        if (!service_is(*received, positive(kServiceTransferData)))
        {
            error(ctx, std::format("TransferData to 0x{:x} rejected: {}", start,
                                   nrc_context(*received)));
            return fail(ErrorKind::BadResponse, "TransferData rejected");
        }
        payload_done += static_cast<std::uint32_t>(chunk.size() - 1);
        if (progress != nullptr)
        {
            progress->update(static_cast<int>(payload_done));
        }
    }

    // Lines 262-270.
    received = exchange(ctx, family.request_id,
                        buildRequestDownload(kCrcTransferAddress, kCrcTransferSize), 50,
                        kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, positive(kServiceRequestDownload)))
    {
        error(ctx,
              std::format("RequestDownload for checksum rejected: {}", nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "checksum RequestDownload rejected");
    }

    // Lines 272-284: big-endian 16-bit running sum, always exactly one
    // TransferData frame (kCrcTransferSize is 2, well under kTransferChunkSize).
    const std::uint16_t crc = checksum(data);
    const bytes::Bytes crc_data{static_cast<bytes::Byte>((crc >> 8) & 0xff),
                                static_cast<bytes::Byte>(crc & 0xff)};
    received = exchange(ctx, family.request_id, buildTransferDataFrames(crc_data).front(), 50,
                        kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, positive(kServiceTransferData)))
    {
        error(ctx,
              std::format("TransferData for checksum rejected: {}", nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "checksum TransferData rejected");
    }

    // Lines 286-294: the CRC check gets the extra-long timeout.
    received = exchange(ctx, family.request_id, buildRoutineCheckCrc(start), 200,
                        kExtraLongTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, positive(kServiceRoutineControl)))
    {
        error(ctx, std::format("RoutineControl CRC check for 0x{:x} rejected: {}", start,
                               nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "CRC RoutineControl rejected");
    }

    if (progress != nullptr)
    {
        progress->complete();
    }

    return {};
}

// Legacy: the unlock + erase-trigger pair that appears identically in
// ensureTopRegionWritten (lines 349-368) and write_mem (lines 445-464). The
// only difference between the two copies is the log-message prefix, so it is
// a parameter here rather than two transcriptions.
Status unlock_and_erase(Ctx& ctx, const MitsuColtM32rCanPlan& family,
                        std::string_view unlock_prefix, std::string_view erase_prefix)
{
    using namespace MitsuColtCan;

    Result<bytes::Bytes> received =
        exchange(ctx, family.request_id, buildRequestReflashUnlock(), 200, kExtraLongTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, positive(kServiceRequestReflash)))
    {
        error(ctx, std::format("{}{}", unlock_prefix, nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "reflash unlock rejected");
    }

    received = exchange(ctx, family.request_id, buildRoutineErase(), 200, kExtraLongTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, positive(kServiceRoutineControl)))
    {
        error(ctx, std::format("{}{}", erase_prefix, nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "erase trigger rejected");
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
Status ensure_top_region_written(Ctx& ctx, const FlashPlan& plan,
                                 const MitsuColtM32rCanPlan& family, bytes::ByteView rom,
                                 PhaseSequence& phases)
{
    using namespace MitsuColtCan;

    PhaseReporter phase = phases.start("Ensure top region", 3);

    // Line 303.
    info(ctx, std::format("Checking top 128KB (0x{:x}-0x{:x})...", kTopRegionStart,
                          kTopRegionEnd));

    // Lines 305-309.
    Result<bytes::Bytes> current_top =
        read_flash_range(ctx, family, kTopRegionStart, kTopRegionLength);
    if (!current_top)
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
    if (Status uploaded =
            upload_and_commit(ctx, family, kEraseRoutineRamAddr, kEraseRedirectRoutine);
        !uploaded)
    {
        error(ctx, "Erase redirect routine upload failed");
        return uploaded;
    }

    // Lines 342-347.
    info(ctx, std::format("Uploading write redirect routine to RAM 0x{:x}...",
                          kWriteRoutineRamAddr));
    if (Status uploaded =
            upload_and_commit(ctx, family, kWriteRoutineRamAddr, kWriteRedirectRoutine);
        !uploaded)
    {
        error(ctx, "Write redirect routine upload failed");
        return uploaded;
    }

    // Lines 349-368.
    if (Status erased = unlock_and_erase(
            ctx, family, "Reflash unlock (top 128KB bootstrap) rejected: ",
            "Erase trigger (top 128KB bootstrap) rejected: ");
        !erased)
    {
        return erased;
    }
    info(ctx, "Carrier window erased");

    // Lines 370-375. The carrier address is kUserspaceStart, not
    // kTopRegionStart: the bootloader hard-validates RequestDownload targets
    // into the userspace window, and the redirect routines add the +0x058000
    // offset themselves. See mitsu_colt_can_protocol.h's kEraseRedirectRoutine
    // comment.
    if (Status written =
            upload_and_commit(ctx, family, kUserspaceStart, wanted_top);
        !written)
    {
        error(ctx, "Top 128KB redirect write failed");
        return written;
    }
    info(ctx, "Top 128KB written via redirect");
    phase.update(2);

    // Lines 377-387.
    Result<bytes::Bytes> verify_top =
        read_flash_range(ctx, family, kTopRegionStart, kTopRegionLength);
    if (!verify_top)
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
Status write_mem(Ctx& ctx, const FlashPlan& plan, const MitsuColtM32rCanPlan& family,
                 bytes::ByteView rom, PhaseSequence& phases)
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
        if (Status bootstrapped = ensure_top_region_written(ctx, plan, family, rom, phases);
            !bootstrapped)
        {
            return bootstrapped;
        }
    }

    PhaseReporter prepare = phases.start("Prepare userspace", 2);

    // Lines 409-415.
    info(ctx,
         std::format("Uploading erase-page routine to RAM 0x{:x}...", kEraseRoutineRamAddr));
    if (Status uploaded =
            upload_and_commit(ctx, family, kEraseRoutineRamAddr, kErasePageRoutine);
        !uploaded)
    {
        error(ctx, "Erase-page routine upload failed");
        return uploaded;
    }
    info(ctx, "Erase page uploaded");
    prepare.update(1);

    // Lines 417-423.
    info(ctx,
         std::format("Uploading write-page routine to RAM 0x{:x}...", kWriteRoutineRamAddr));
    if (Status uploaded =
            upload_and_commit(ctx, family, kWriteRoutineRamAddr, kWritePageRoutine);
        !uploaded)
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
    if (Status erased = unlock_and_erase(ctx, family, "Reflash unlock rejected: ",
                                         "Erase trigger rejected: ");
        !erased)
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
    if (Status written =
            upload_and_commit(ctx, family, kUserspaceStart, userspace, &write);
        !written)
    {
        error(ctx, "ROM userspace write failed");
        return written;
    }
    info(ctx, "Userspace flash written");

    PhaseReporter verify =
        phases.start("Verify userspace", static_cast<int>(userspace.size()) + 1);
    Result<bytes::Bytes> verify_userspace =
        read_flash_range(ctx, family, kUserspaceStart, page_write_end - kUserspaceStart,
                         &verify);
    if (!verify_userspace)
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
    if (Status matched = check_family_transport_match(plan, FlashFamily::MitsuColtM32rCan,
                                                      TransportKind::CanIso15765);
        !matched)
    {
        return std::unexpected(matched.error());
    }
    if (Status valid = validate_mitsu_colt_m32r_can_plan(plan); !valid)
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

    Ctx ctx{*can, clock, cancellation, events};
    const std::uint32_t rom_end =
        plan.transfer_region().start + plan.transfer_region().length;
    const bool read = plan.operation() == FlashOperation::Read;
    PhaseSequence phases(events, read ? 2 : (rom_end == MitsuColtCan::kFullRomSize ? 6 : 5));
    PhaseReporter connect = phases.start(read ? "Connect to ECU" : "Connect", 1);

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
        Result<bytes::Bytes> rom = read_flash_range(ctx, family, plan.transfer_region().start,
                                                    plan.transfer_region().length, &read_phase);
        if (!rom)
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
    if (Status written = write_mem(ctx, plan, family, *plan.image(), phases); !written)
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{
        .operation = plan.operation(),
        .read_bytes = std::nullopt,
    };
}

} // namespace fastecu::flash
