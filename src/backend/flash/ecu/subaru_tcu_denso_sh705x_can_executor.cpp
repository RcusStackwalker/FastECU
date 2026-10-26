#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/beef_can_kernel.h"
#include "src/backend/flash/ecu/beef_can_session.h"
#include "src/backend/flash/ecu/denso_beef_can_common.h"
#include "src/backend/flash/ecu/denso_iso15765_crypto.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/flash/transfer_progress.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every protocol exchange below is transcribed from
// src/platform/desktop/common/flash/legacy/tcu/
// flash_tcu_subaru_denso_sh705x_can_operation.cpp at revision 59f4e442:
// connect_bootloader (93-364), upload_kernel (369-627), read_mem (632-779),
// write_mem (784-900), CRC/init/reflash/flash (905-1517), seed/crypto
// (1522-1577), and request_kernel_id (1583-1647).
//
// ISO-15765 is only the envelope transport here. Strict UDS exchanges use
// uds::UdsClient; the tolerant 0xAA/0x09 identity queries and all BEEF
// kernel commands are sent through the channel directly because they are
// vendor/proprietary messages rather than UDS application PDUs.

namespace fastecu::flash
{
namespace
{

using bytes::ComposeBe;
using namespace bytes::literals;
using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kProbeDelay{100}; // request_kernel_id(), lines 1608-1609
constexpr std::chrono::milliseconds kExtraShortTimeout{50};
constexpr std::chrono::milliseconds kShortTimeout{200};
constexpr std::chrono::milliseconds kMediumTimeout{500};
constexpr std::chrono::milliseconds kLongTimeout{800};
constexpr std::chrono::milliseconds kReadTimeout{2000};
constexpr std::chrono::milliseconds kExtraLongTimeout{3000};
constexpr std::chrono::milliseconds kPostUploadDelay{500}; // upload_kernel(), lines 595-595

constexpr int kKernelPageSize = 0x400;
constexpr int kFlashBufferSize = 0x200;
constexpr int kFlashCommitSize = 0x1000;

constexpr bytes::Byte kKernelId = 0x41;
constexpr bytes::Byte kKernelReadArea = 0x03;
constexpr bytes::Byte kKernelProgVolt = 0x04;
constexpr bytes::Byte kKernelWriteBuffer = 0x22;
constexpr bytes::Byte kKernelCommitBuffer = 0x24;
constexpr bytes::Byte kKernelBlankPage = 0x25;

constexpr uds::ExchangePolicy kStrictPolicy{
    .pre_read_delay = kExtraShortTimeout,
    .read_timeout = kReadTimeout,
    .pending_timeout = kExtraLongTimeout,
    .max_pending_repeats = 10,
};

struct Context
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    IClock& clock;
    ICanFlashTransport& transport;
    std::uint32_t request_id;
    std::uint32_t response_id;
    uds::UdsClient& uds;
    uds::IUdsChannel& channel;
    BeefCanSession session;
    BeefCanKernel kernel;
};

bytes::Bytes SeedKey(bytes::ByteView seed)
{
    return DensoSeedKey(seed);
}

bool IsFatal(const Error& error)
{
    return error.kind == ErrorKind::kCancelled || error.kind == ErrorKind::kDisconnected;
}

// A malformed kernel ID reply means "no kernel" to a tolerant probe and fails
// a strict one.
Result<std::optional<std::string>> Malformed(bool tolerate_malformed, Error error)
{
    if (tolerate_malformed)
    {
        return std::optional<std::string>{};
    }
    return std::unexpected(std::move(error));
}

Status SendKernelIdProbe(Context& context, bytes::ByteView request, bytes::ByteView wire_request)
{
    if (const Status checkpoint = CancelledIfRequested(context, "kernel ID probe cancelled"); !checkpoint.has_value())
    {
        return checkpoint;
    }
    if (const Status sent = context.channel.Send(request, context.cancellation); !sent.has_value())
    {
        return sent;
    }
    LogInfo(context, std::format("Kernel ID request: {}", bytes::ToHex(wire_request)));
    return context.clock.Sleep(kProbeDelay, context.cancellation);
}

// Legacy lines 1621-1625 keep reading at the short timeout and append every
// returned continuation. ICanFlashTransport replaces the legacy adapter's
// two-byte prefix with a four-byte arbitration-id envelope, so each accepted
// continuation contributes every byte after that envelope. The drain is
// bounded and stays cancellation and disconnect aware; false means the bound
// ran out before the queue emptied.
Result<bool> DrainKernelIdFragments(Context& context, bytes::Bytes& coalesced)
{
    constexpr int kMaxKernelIdTrailingFrames = 32;
    for (int trailing = 0; trailing < kMaxKernelIdTrailingFrames; ++trailing)
    {
        if (const Status checkpoint = CancelledIfRequested(context, "kernel ID trailing drain cancelled");
            !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        Result<std::optional<bytes::Bytes>> fragment = context.transport.Read(kShortTimeout, context.cancellation);
        if (!fragment.has_value())
        {
            if (fragment.error().kind == ErrorKind::kTimeout)
            {
                return true;
            }
            return std::unexpected(fragment.error());
        }
        if (!fragment->has_value())
        {
            return true;
        }
        const bytes::Bytes& fragment_raw = **fragment;
        if (fragment_raw.size() < CanFlashUdsChannel::kEnvelopeSize ||
            bytes::ReadU32Be(fragment_raw) != context.response_id)
        {
            continue;
        }
        coalesced.insert(coalesced.end(),
                         fragment_raw.begin() + static_cast<std::ptrdiff_t>(CanFlashUdsChannel::kEnvelopeSize),
                         fragment_raw.end());
    }
    return false;
}

Result<std::optional<std::string>> ParseKernelIdReply(Context& context, const bytes::Bytes& raw,
                                                      bool tolerate_malformed)
{
    if (raw.size() < CanFlashUdsChannel::kEnvelopeSize)
    {
        return Malformed(tolerate_malformed, Error{ErrorKind::kBadResponse, "short CAN frame in kernel ID response"});
    }
    if (bytes::ReadU32Be(raw) != context.response_id)
    {
        return Malformed(tolerate_malformed, Error{ErrorKind::kBadResponse, "wrong CAN id in kernel ID response"});
    }
    bytes::Bytes coalesced(raw.begin() + static_cast<std::ptrdiff_t>(CanFlashUdsChannel::kEnvelopeSize), raw.end());
    Result<bool> drained = DrainKernelIdFragments(context, coalesced);
    if (!drained.has_value())
    {
        return IsFatal(drained.error()) ? std::unexpected(drained.error())
                                        : Malformed(tolerate_malformed, drained.error());
    }
    if (!*drained)
    {
        // Never begin another command while the raw receive queue may still
        // contain ID fragments. This is the bounded portable form of the
        // legacy read-until-empty loop.
        return Malformed(tolerate_malformed,
                         Error{ErrorKind::kBadResponse, "kernel ID trailing drain exceeded its frame bound"});
    }
    Result<BeefMessage> parsed = ParseBeef(coalesced);
    if (!parsed.has_value())
    {
        return Malformed(tolerate_malformed, parsed.error());
    }
    if (parsed->opcode != kKernelId)
    {
        LogError(context, std::format("Wrong response from ECU: {}", bytes::ToHex(coalesced)));
        return Malformed(tolerate_malformed, Error{ErrorKind::kBadResponse, "unexpected kernel ID response opcode"});
    }
    return std::optional<std::string>{std::string(parsed->payload.begin(), parsed->payload.end())};
}

Result<std::optional<std::string>> RequestKernelId(Context& context, bool tolerate_malformed)
{
    // request_kernel_id(), revision 59f4e442 lines 1583-1647. The literal
    // PDU is intentionally kept here instead of deriving it from BEEF
    // constants: 0x7A 0xA0 is the legacy kernel probe, not a BEEF command.
    const bytes::Bytes request{0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const bytes::Bytes wire_request = ComposeBe(context.request_id, request);
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (const Status sent = SendKernelIdProbe(context, request, wire_request); !sent.has_value())
        {
            return std::unexpected(sent.error());
        }
        Result<std::optional<bytes::Bytes>> received = context.transport.Read(kLongTimeout, context.cancellation);
        LogInfo(context,
                std::format("Kernel ID response: {}",
                            received.has_value() && received->has_value() ? bytes::ToHex(**received) : std::string{}));
        if (!received.has_value())
        {
            if (received.error().kind == ErrorKind::kTimeout)
            {
                continue;
            }
            return IsFatal(received.error()) ? std::unexpected(received.error())
                                             : Malformed(tolerate_malformed, received.error());
        }
        if (received->has_value())
        {
            return ParseKernelIdReply(context, **received, tolerate_malformed);
        }
    }
    return std::optional<std::string>{};
}

Status StrictPayload(Context& context, bytes::ByteView pdu, bytes::ByteView expected, std::string_view label,
                     std::optional<std::size_t> minimum_size = std::nullopt)
{
    Result<bytes::Bytes> reply = context.uds.Request(pdu, kStrictPolicy, context.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    const std::size_t required = minimum_size.value_or(expected.size());
    if (payload.size() < required || !std::equal(expected.begin(), expected.end(), payload.begin()))
    {
        LogError(context, std::format("Wrong response from TCU during {}", label));
        return Fail(ErrorKind::kBadResponse, std::format("unexpected response during {}", label));
    }
    return {};
}

Status StrictSeed(Context& context, bytes::Bytes& key)
{
    Result<bytes::Bytes> seed_reply = context.uds.Request(
        bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, kStrictPolicy, context.cancellation);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*seed_reply);
    if (payload.size() < 5 || payload[0] != uds::kSecurityAccessRequestSeed)
    {
        return Fail(ErrorKind::kBadResponse, "invalid TCU seed response");
    }
    key = SeedKey(payload.subspan(1, 4));
    return {};
}

// The ECU/CAL identity queries (lines 137-214) are intentionally non-fatal.
// Their successful values form the legacy read ROM id.
Status RequestIdentity(Context& context, bool read_operation, std::optional<std::string>& rom_id)
{
    if (Status ecu_id = context.session.RequestEcuId(read_operation, rom_id); !ecu_id.has_value())
    {
        return ecu_id;
    }
    LogInfo(context, "Requesting CAL ID");
    Result<std::optional<std::string>> cal_id = context.session.IdentityString(
        bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId}, 0x49, 0x04, "CAL ID");
    if (!cal_id.has_value())
    {
        return std::unexpected(cal_id.error());
    }
    if (read_operation && cal_id->has_value())
    {
        rom_id = **cal_id + "_" + rom_id.value_or(std::string{});
    }
    return {};
}

// Strict UDS session/security exchanges, lines 216-359. Oracle line 230
// joins its two mismatches with && and therefore accepts 50 02 or 51 03;
// requiring both fields is the documented automated-only safety fix.
Status EnterProgrammingSession(Context& context)
{
    LogInfo(context, "Requesting session mode");
    if (const Status session = StrictPayload(context, bytes::Bytes{uds::kSidDiagnosticSessionControl, 0x03},
                                             bytes::Bytes{0x03}, "session mode");
        !session.has_value())
    {
        return session;
    }
    bytes::Bytes key;
    if (const Status seed = StrictSeed(context, key); !seed.has_value())
    {
        return seed;
    }
    LogInfo(context, "Seed request ok");
    LogInfo(context, "Sending seed key");
    bytes::Bytes key_request{uds::kSidSecurityAccess, uds::kSecurityAccessSendKey};
    key_request.insert(key_request.end(), key.begin(), key.end());
    if (const Status key_status = StrictPayload(context, key_request, bytes::Bytes{0x02}, "seed key");
        !key_status.has_value())
    {
        return key_status;
    }
    LogInfo(context, "Seed key ok");
    return EnterBeefProgrammingSession(context, true, false, kStrictPolicy);
}

Status ConnectBootloader(Context& context, bool read_operation, std::optional<std::string>& rom_id)
{
    // connect_bootloader(), revision 59f4e442 lines 107-135. The oracle
    // continues initialization after any nonterminal invalid probe reply;
    // post-upload callers select the strict path instead.
    if (Status identity = RequestIdentity(context, read_operation, rom_id); !identity.has_value())
    {
        return identity;
    }
    return EnterProgrammingSession(context);
}

Status UploadKernel(Context& context, const KernelImage& kernel)
{
    if (const Status uploaded = context.kernel.TransferPayload(kernel, {kStrictPolicy, kReadTimeout});
        !uploaded.has_value())
    {
        return uploaded;
    }
    LogInfo(context, "Kernel uploaded, starting...");
    if (const Status transfer_exit =
            StrictPayload(context, bytes::Bytes{uds::kSidRequestTransferExit}, {}, "kernel transfer exit");
        !transfer_exit.has_value())
    {
        return transfer_exit;
    }
    // upload_kernel(), revision 59f4e442 lines 546-577 checks only the
    // positive service id, so an echoed routine is optional.
    if (const Status start_kernel =
            StrictPayload(context, bytes::Bytes{uds::kSidRoutineControl, 0x01, 0x02, 0x02, 0x02}, {}, "kernel start");
        !start_kernel.has_value())
    {
        return start_kernel;
    }

    LogInfo(context, "Kernel started, initializing...");
    // upload_kernel(), revision 59f4e442 lines 581-595 resets the adapter
    // and reapplies the TCU's ISO-15765 configuration before its quiet
    // period and strict post-upload probe. This is protocol-owned inner
    // lifecycle; BoundAttempt retains the initial configure/open and final
    // close ownership.
    if (const Status restarted = context.transport.RestartIso15765({.bitrate = 500000,
                                                                    .request_id = context.request_id,
                                                                    .response_id = context.response_id,
                                                                    .extended_id = false},
                                                                   context.cancellation);
        !restarted.has_value())
    {
        return restarted;
    }
    if (const Status slept = context.clock.Sleep(kPostUploadDelay, context.cancellation); !slept.has_value())
    {
        return slept;
    }
    LogInfo(context, "Requesting kernel ID");
    Result<std::optional<std::string>> kernel_id = RequestKernelId(context, false);
    if (!kernel_id.has_value())
    {
        return std::unexpected(kernel_id.error());
    }
    if (kernel_id->has_value())
    {
        LogInfo(context, std::format("Kernel ID: {}", **kernel_id));
    }
    else
    {
        LogError(context, "No valid response from ECU");
        return Fail(ErrorKind::kTimeout, "kernel did not answer after upload");
    }
    return {};
}

Result<bytes::Bytes> ReadMemory(Context& context, const FlashPlan& plan, PhaseReporter& progress)
{
    // read_mem(), revision 59f4e442 lines 632-779. The kernel reads fixed
    // 0x400-byte pages and appends each raw BEEF payload for the caller.
    const MemoryRegion region = plan.TransferRegion();
    bytes::Bytes rom;
    rom.reserve(region.length);
    LogInfo(context, "Start reading ROM, please wait...");
    for (std::uint32_t offset = 0; offset < region.length; offset += kKernelPageSize)
    {
        const auto loop_started = context.clock.Now();
        if (const Status checkpoint = CancelledIfRequested(context, "read cancelled"); !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        const std::uint32_t address = region.start + offset;
        const auto payload = ComposeBe(bytes::Byte{0x00}, bytes::U24{address}, std::uint16_t{kKernelPageSize});
        Result<bytes::Bytes> reply = context.session.Exchange(kKernelReadArea, payload, kKernelPageSize, kReadTimeout);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        const bytes::ByteView raw_page = bytes::ByteView(*reply).first(kKernelPageSize);
        bytes::Bytes page(raw_page.begin(), raw_page.end());
        if (page.size() > region.length - offset)
        {
            page.resize(region.length - offset);
        }
        const std::uint64_t elapsed_ms = ElapsedMilliseconds(loop_started, context.clock.Now());
        const TransferRate rate = ComputeTransferRate(kKernelPageSize, elapsed_ms, region.length - offset);
        LogInfo(context, FormatReadProgress(address, kKernelPageSize, rate));
        rom.insert(rom.end(), page.begin(), page.end());
        progress.Update(static_cast<int>(std::min<std::uint64_t>(region.length, offset + kKernelPageSize)));
    }
    LogInfo(context, "ROM read ready");
    return rom;
}

Status FlashBlock(Context& context, bytes::ByteView image, const FlashPlan& plan, const MemoryRegion& block,
                  std::size_t& flash_bytes_index, std::size_t flash_bytes_count, PhaseReporter& progress)
{
    // flash_block(), revision 59f4e442 lines 1270-1515. The legacy always
    // uses 0x200-byte buffer writes and commits each 0x1000-byte page.
    if (block.length == 0 || block.length % kFlashBufferSize != 0 || block.length % kFlashCommitSize != 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "TCU flash block is not aligned to the portable write windows");
    }
    const std::size_t image_offset = block.start - plan.TransferRegion().start;
    std::uint32_t start = block.start;
    std::uint32_t remaining = block.length;
    std::uint32_t commit_start = block.start;
    std::chrono::steady_clock::time_point previous_time;

    LogInfo(context, std::format("Flash page erase addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    LogInfo(context, "Erasing flash page...");
    const auto erase_payload = ComposeBe(block.start);
    Result<bytes::Bytes> erased = context.session.Exchange(kKernelBlankPage, erase_payload, 0, kExtraLongTimeout);
    if (!erased.has_value())
    {
        return std::unexpected(erased.error());
    }
    LogInfo(context, " erased");
    previous_time = context.clock.Now();

    LogInfo(context, std::format("Start flash write addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    while (remaining != 0)
    {
        if (const Status checkpoint = CancelledIfRequested(context, "flash write cancelled"); !checkpoint.has_value())
        {
            return checkpoint;
        }
        const std::size_t chunk_offset = static_cast<std::size_t>(start - block.start);
        const bytes::ByteView chunk = image.subspan(image_offset + chunk_offset, kFlashBufferSize);
        const bytes::Bytes write_payload = ComposeBe(start, chunk);
        Result<bytes::Bytes> written = context.session.Exchange(kKernelWriteBuffer, write_payload, 0, kLongTimeout);
        if (!written.has_value())
        {
            return std::unexpected(written.error());
        }
        LogDebug(context, "Data written to flash buffer");

        const std::uint32_t percent =
            static_cast<std::uint32_t>(100U * (block.length - remaining) / std::max<std::uint32_t>(block.length, 1));
        const auto now = context.clock.Now();
        const std::uint64_t elapsed = ElapsedMilliseconds(previous_time, now);
        previous_time = now;
        const auto next_index = static_cast<std::uint32_t>(flash_bytes_index + kFlashBufferSize);
        const TransferRate rate = ComputeTransferRate(kFlashBufferSize, elapsed, flash_bytes_count - next_index);
        LogInfo(context, FormatWriteProgress(start, percent, rate));

        remaining -= kFlashBufferSize;
        start += kFlashBufferSize;
        flash_bytes_index += kFlashBufferSize;
        progress.Update(static_cast<int>(std::min<std::size_t>(flash_bytes_index, flash_bytes_count)));

        if (start - commit_start == kFlashCommitSize)
        {
            if (const Status checkpoint = CancelledIfRequested(context, "flash commit cancelled");
                !checkpoint.has_value())
            {
                return checkpoint;
            }
            LogInfo(context, "Flash buffer write complete... ");
            const std::size_t crc_offset = image_offset + static_cast<std::size_t>(commit_start - block.start);
            const std::uint32_t image_crc = fastecu::checksum::Crc32(image.subspan(crc_offset, kFlashCommitSize));
            LogDebug(context, std::format("Image CRC32: 0x{:08x}", image_crc));
            LogInfo(context, std::format("Committ flash addr: 0x{:x}", commit_start));
            LogInfo(context, std::format(" len: 0x{:x}", kFlashCommitSize));
            LogInfo(context, std::format(" crc32: 0x{:08x}", image_crc));
            const auto commit_payload = ComposeBe(commit_start, std::uint16_t{kFlashCommitSize}, image_crc);
            Result<bytes::Bytes> committed =
                context.session.Exchange(kKernelCommitBuffer, commit_payload, 0, kExtraLongTimeout);
            if (!committed.has_value())
            {
                return std::unexpected(committed.error());
            }
            commit_start += kFlashCommitSize;
        }
    }
    return {};
}

Status ReflashBlock(Context& context, bytes::ByteView image, const FlashPlan& plan, const MemoryRegion& block,
                    std::size_t& flash_bytes_index, std::size_t flash_bytes_count, PhaseReporter& progress)
{
    // reflash_block(), revision 59f4e442 lines 1183-1267. The voltage query
    // precedes flash_block and a failure after erase receives the exact
    // operator recovery warning from the legacy worker.
    LogInfo(context, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    LogInfo(context, "Check flash voltage");
    Result<bytes::Bytes> voltage = context.session.Exchange(kKernelProgVolt, {}, 2, kMediumTimeout);
    if (!voltage.has_value())
    {
        return std::unexpected(voltage.error());
    }
    const double volts = static_cast<double>(bytes::ReadU16Be(*voltage)) / 50.0;
    LogInfo(context, std::format(": {}V", volts));

    Status flashed = FlashBlock(context, image, plan, block, flash_bytes_index, flash_bytes_count, progress);
    if (!flashed.has_value())
    {
        LogError(context,
                 "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most likely still "
                 "running and receiving commands!");
        return flashed;
    }
    LogInfo(context, "Flash block ok");
    return {};
}

Status WriteMemory(Context& context, const FlashPlan& plan, PhaseSequence& phases, PhaseReporter& compare_progress)
{
    // write_mem(), revision 59f4e442 lines 784-900.
    const bytes::ByteView image = plan.ImageOrEmpty();
    Result<CompareResult> before = context.session.CompareBlocks(plan, image, &compare_progress);
    if (!before.has_value())
    {
        return std::unexpected(before.error());
    }
    compare_progress.Complete();
    if (before->changed_count == 0)
    {
        LogInfo(context, "*** Compare results no difference between ROM and ECU data, no flashing needed! ***");
        phases.Start("Write", 0);
        return {};
    }

    std::size_t flash_bytes_count = 0;
    for (std::size_t index = 0; index < before->modified.size(); ++index)
    {
        if (before->modified[index])
        {
            flash_bytes_count += plan.EraseRegions()[index].length;
        }
    }
    LogInfo(context, "--- Start writing ROM file to ECU flash memory ---");
    if (const Status initialized = context.session.InitializeFlash(false); !initialized.has_value())
    {
        return initialized;
    }
    PhaseReporter write_progress = phases.Start("Write", static_cast<int>(flash_bytes_count));
    std::size_t flash_bytes_index = 0;
    for (std::size_t index = 0; index < before->modified.size(); ++index)
    {
        if (!before->modified[index])
        {
            continue;
        }
        if (const Status checkpoint = CancelledIfRequested(context, "flash block loop cancelled");
            !checkpoint.has_value())
        {
            return checkpoint;
        }
        if (const Status reflashed = ReflashBlock(context, image, plan, plan.EraseRegions()[index], flash_bytes_index,
                                                  flash_bytes_count, write_progress);
            !reflashed.has_value())
        {
            LogInfo(context, std::format("Block {} reflash failed.", index));
            return reflashed;
        }
        LogInfo(context, std::format("Block {} reflash complete.", index));
    }
    write_progress.Complete();

    Result<CompareResult> after = context.session.CompareBlocks(plan, image, nullptr, true);
    if (!after.has_value())
    {
        return std::unexpected(after.error());
    }
    if (after->changed_count != 0)
    {
        LogError(context, "*** ERROR IN FLASH PROCESS ***");
        LogError(context, "Don't power off your ECU, kernel is still running and you can try flashing again!");
    }
    return {};
}

} // namespace

Result<Iso15765Config> SubaruTcuDensoSh705xCanExecutor::Setup(const SubaruTcuDensoSh705xCanPlan& family) const
{
    return Iso15765ConfigFrom(family);
}

Result<FlashExecutionResult> SubaruTcuDensoSh705xCanExecutor::Run(const SubaruTcuDensoSh705xCanPlan& family,
                                                                  const FlashPlan& plan, ICanFlashTransport& transport,
                                                                  IClock& clock, const ICancellationToken& cancellation,
                                                                  IEventSink& events)
{
    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Context context{
        cancellation,
        events,
        clock,
        transport,
        family.request_id,
        family.response_id,
        uds_client,
        channel,
        BeefCanSession(transport, clock, cancellation, events, channel, kSubaruTcuDensoSh705xCanBeefProfile),
        BeefCanKernel(transport, clock, cancellation, events, channel, kSubaruTcuDensoSh705xCanBeefProfile)};

    return RunBeefKernelOperation(
        plan, "Subaru Denso SH705x TCU", context, [](Context& c) { return RequestKernelId(c, true); },
        ConnectBootloader, UploadKernel, ReadMemory, WriteMemory);
}

} // namespace fastecu::flash
