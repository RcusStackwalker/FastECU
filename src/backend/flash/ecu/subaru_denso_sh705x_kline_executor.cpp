#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/diagnostics/nrc_parser.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/denso_sh705x_kline_common.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/transfer_progress.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/logged_step.h"

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::ComposeBeWithChecksum;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;
using OptionalBytes = IKlineFlashTransport::OptionalBytes;

// Legacy: src/platform/desktop/common/flash/legacy/ecu/
// flash_ecu_subaru_denso_sh705x_kline_operation.{h,cpp}, deleted when this executor replaced them.
constexpr std::uint16_t kStartComm = 0xBEEF; // kernelcomms.h SUB_KERNEL_START_COMM
constexpr std::uint8_t kOpId = 0x01;
constexpr std::uint8_t kOpCrc = 0x02;
constexpr std::uint8_t kOpReadArea = 0x03;
constexpr std::uint8_t kOpProgVolt = 0x04;
constexpr std::uint8_t kOpGetMaxMsgSize = 0x05;
constexpr std::uint8_t kOpGetMaxBlockSize = 0x06;
constexpr std::uint8_t kOpFlashEnable = 0x20;
constexpr std::uint8_t kOpFlashDisable = 0x21;
constexpr std::uint8_t kOpWriteFlashBuffer = 0x22;
constexpr std::uint8_t kOpValidateFlashBuffer = 0x23;
constexpr std::uint8_t kOpCommitFlashBuffer = 0x24;
constexpr std::uint8_t kOpBlankPage = 0x25;

constexpr std::chrono::milliseconds kReadTimeout = 2000ms;      // serial_read_timeout
constexpr std::chrono::milliseconds kShortTimeout = 200ms;      // serial_read_short_timeout
constexpr std::chrono::milliseconds kMediumTimeout = 500ms;     // serial_read_medium_timeout
constexpr std::chrono::milliseconds kLongTimeout = 800ms;       // serial_read_long_timeout
constexpr std::chrono::milliseconds kExtraLongTimeout = 3000ms; // serial_read_extra_long_timeout

constexpr int kProbeBaud = 62500;
constexpr int kSsmBaud = 4800;
constexpr int kUploadBaud = 15625;
constexpr std::chrono::milliseconds kBaudSettle = 100ms;
constexpr std::chrono::milliseconds kKernelIdSettle = 200ms;   // request_kernel_id() delay(200)
constexpr std::chrono::milliseconds kPostStartRoutine = 100ms; // upload_kernel() delay(100) before 62500
constexpr std::chrono::milliseconds kCrcBlockPacing = 5ms;     // get_changed_blocks() delay(5)

constexpr std::uint32_t kReadPageSize = 0x400;     // read_mem() pagesize
constexpr std::uint32_t kWriteChunkSize = 0x200;   // flash_block() blocksize
constexpr std::uint32_t kCommitBlockSize = 0x1000; // flash_block() flashblocksize
constexpr std::uint32_t kUploadChunkBytes = 0x80;  // send_sid_36_transferdata() blocksize

// Every kernel request, e.g. request_kernel_id() and check_romcrc():
// BE EF, u16 length (opcode + payload), opcode, payload, sum8.
bytes::Bytes Frame(std::uint8_t opcode, bytes::ByteView payload = {})
{
    return ComposeBeWithChecksum(bytes::Sum8, kStartComm, static_cast<std::uint16_t>(payload.size() + 1),
                                 bytes::Byte(opcode), payload);
}

// Legacy kernel-reply checks index at(0), at(1) and at(4) after a length check.
bool KernelReplyOk(bytes::ByteView received, std::uint8_t opcode, std::size_t min_size)
{
    return received.size() >= min_size && received[0] == 0xBE && received[1] == 0xEF &&
           received[4] == static_cast<bytes::Byte>(opcode | 0x40U);
}

// write -> [settle] -> read, cancellation-checked at every boundary.
Result<OptionalBytes> Exchange(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                               bytes::ByteView request, std::chrono::milliseconds settle,
                               std::chrono::milliseconds timeout)
{
    return CheckCancelled(cancellation, "cancelled before write")
        .and_then([&] { return transport.Write(request); })
        .and_then(
            [&](std::size_t written) -> Status
            {
                if (written != request.size())
                {
                    return Fail(ErrorKind::kDisconnected, "short K-Line write");
                }
                return settle > 0ms ? clock.Sleep(settle, cancellation) : Status{};
            })
        .and_then([&] { return CheckCancelled(cancellation, "cancelled before read"); })
        .and_then([&] { return transport.Read(timeout, cancellation); })
        .and_then(
            [&](OptionalBytes received)
            {
                return CheckCancelled(cancellation, "cancelled after read")
                    .transform([&] { return std::move(received); });
            });
}

// Like exchange(), but no frame at all is a Timeout -- legacy treated an
// empty reply as "No valid response from ECU" and stopped.
Result<bytes::Bytes> RequiredExchange(IKlineFlashTransport& transport, IClock& clock,
                                      const ICancellationToken& cancellation, bytes::ByteView request,
                                      std::chrono::milliseconds settle, std::chrono::milliseconds timeout,
                                      std::string_view what)
{
    Result<OptionalBytes> received = Exchange(transport, clock, cancellation, request, settle, timeout);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, std::format("no response from ECU during {}", what));
    }
    return std::move(**received);
}

Status ChangeBaud(IKlineFlashTransport& transport, const ICancellationToken& cancellation, int baud)
{
    return CheckCancelled(cancellation, "cancelled before changing baud")
        .and_then([&] { return transport.SetBaud(baud); });
}

Status SleepFor(IClock& clock, const ICancellationToken& cancellation, std::chrono::milliseconds duration)
{
    return clock.Sleep(duration, cancellation);
}

// Legacy: emit LOG_E("Wrong response from ECU: " +
// FileActions::parse_nrc_message(received.mid(offset, ...))), which is
// nrc_description() over everything from `offset` on (empty past the end).
void LogWrongResponse(IEventSink& events, bytes::ByteView received, std::size_t offset)
{
    const bytes::ByteView nrc = offset < received.size() ? received.subspan(offset) : bytes::ByteView{};
    events.Log(LogLevel::kError, std::format("Wrong response from ECU: {}", NrcDescription(nrc)));
}

// received.remove(0, 5); received.remove(received.length() - 1, 1): the
// kernel ID text between the opcode byte and the checksum. A five-byte reply
// passes legacy's `> 4` check and leaves an empty ID.
std::string KernelIdText(bytes::ByteView reply)
{
    return reply.size() > 5 ? std::string(reply.begin() + 5, reply.end() - 1) : std::string{};
}

// Each ECU ID byte as two uppercase hex digits.
std::string EcuIdHex(bytes::ByteView id)
{
    std::string out;
    for (const bytes::Byte byte : id)
    {
        out += std::format("{:02X}", byte);
    }
    return out;
}

// BE EF 00 01 01 sum8, delay(200),
// read_serial_data(serial_read_long_timeout).
Result<OptionalBytes> RequestKernelId(IKlineFlashTransport& transport, IClock& clock,
                                      const ICancellationToken& cancellation)
{
    return Exchange(transport, clock, cancellation, Frame(kOpId), kKernelIdSettle, kLongTimeout);
}

// A live kernel is the only success; any other
// reply is the normal "not running" case and falls through, as in legacy.
Result<bool> ProbeKernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                         IEventSink& events)
{
    // Legacy ignored this result; a failed baud
    // change stops here (see the Task 6 report).
    return ChangeBaud(transport, cancellation, kProbeBaud)
        .and_then([&] { return SleepFor(clock, cancellation, kBaudSettle); })
        .and_then(
            [&]
            {
                events.Log(LogLevel::kInfo, "Checking if kernel is already running...");
                events.Log(LogLevel::kInfo, "Requesting kernel ID");
                return RequestKernelId(transport, clock, cancellation);
            })
        .transform(
            [&](const OptionalBytes& received)
            {
                if (!received.has_value() || received->size() <= 4)
                {
                    events.Log(LogLevel::kError, "No valid response from ECU");
                    return false;
                }
                const bytes::Bytes& reply = *received;
                if (!KernelReplyOk(reply, kOpId, 5))
                {
                    LogWrongResponse(events, reply, 8);
                    return false;
                }
                events.Log(LogLevel::kInfo, std::format("Kernel ID: {}", KernelIdText(reply)));
                return true;
            });
}

// Every send_sid_*() helper: SsmProtocol::addHeader(payload, tester_id,
// target_id), write_serial_data_echo_check(), read_serial_data(timeout); no
// delay. Legacy then logged "No valid response from ECU" when nothing came
// back (the `> N` length check failing on an empty reply).
Result<bytes::Bytes> SsmExchange(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                                 IEventSink& events, const SubaruDensoSh705xKlinePlan& plan, bytes::ByteView payload,
                                 std::chrono::milliseconds timeout, std::string_view what)
{
    Result<bytes::Bytes> reply =
        RequiredExchange(transport, clock, cancellation,
                         ssm_protocol::AddHeader(payload, plan.tester_id, plan.target_id), 0ms, timeout, what);
    if (!reply.has_value() && reply.error().kind == ErrorKind::kTimeout)
    {
        events.Log(LogLevel::kError, "No valid response from ECU");
    }
    return reply;
}

// The legacy reply check shared by every SSM step: `received.length() >
// min_size - 1`, else "No valid response from ECU"; then at(4) (and at(5))
// against the positive response, else "Wrong response from ECU: " + NRC.
Status RequireSsm(const bytes::Bytes& reply, std::size_t min_size, std::uint8_t byte4,
                  std::optional<std::uint8_t> byte5, IEventSink& events)
{
    if (reply.size() < min_size)
    {
        events.Log(LogLevel::kError, "No valid response from ECU");
        return Fail(ErrorKind::kBadResponse, "No valid response from ECU");
    }
    if (reply[4] != byte4 || (byte5.has_value() && reply[5] != *byte5))
    {
        LogWrongResponse(events, reply, 4);
        return Fail(ErrorKind::kBadResponse, "Wrong response from ECU");
    }
    return {};
}

// One SSM step: the exchange, then the legacy reply check.
Result<bytes::Bytes> SsmStep(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                             IEventSink& events, const SubaruDensoSh705xKlinePlan& plan, bytes::ByteView payload,
                             std::string_view what, std::size_t min_size, std::uint8_t byte4,
                             std::optional<std::uint8_t> byte5 = std::nullopt)
{
    return SsmExchange(transport, clock, cancellation, events, plan, payload, kReadTimeout, what)
        .and_then(
            [&](bytes::Bytes reply)
            { return RequireSsm(reply, min_size, byte4, byte5, events).transform([&] { return std::move(reply); }); });
}

Result<std::string> RequestEcuId(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                                 IEventSink& events, const SubaruDensoSh705xKlinePlan& plan)
{
    events.Log(LogLevel::kInfo, "Requesting ECU ID");
    Result<bytes::Bytes> bf =
        SsmStep(transport, clock, cancellation, events, plan, bytes::Bytes{0xBF}, "ECU ID request", 5, 0xFF);
    if (!bf.has_value())
    {
        return std::unexpected(bf.error());
    }
    // Correction: legacy checked only `> 4`, then removed 8 bytes and kept 5;
    // require all 13 before slicing. The positive-reply check
    // above stays first so a negative reply still logs its NRC.
    if (bf->size() < 13)
    {
        events.Log(LogLevel::kError, "No valid response from ECU");
        return Fail(ErrorKind::kBadResponse, "ECU ID reply too short");
    }
    std::string ecu_id = EcuIdHex(bytes::ByteView(*bf).subspan(8, 5));
    events.Log(LogLevel::kInfo, std::format("ECU ID: {}", ecu_id));
    return ecu_id;
}

Status StartCommunication(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                          IEventSink& events, const SubaruDensoSh705xKlinePlan& plan)
{
    events.Log(LogLevel::kInfo, "Requesting to start communication");
    return SsmStep(transport, clock, cancellation, events, plan, bytes::Bytes{0x81}, "start communication", 5, 0xC1)
        .and_then(LogThen(events, "Start communication ok", "Requesting timings params",
                          [&]
                          {
                              return SsmStep(transport, clock, cancellation, events, plan, bytes::Bytes{0x83, 0x00},
                                             "timing parameters", 5, 0xC3);
                          }))
        .transform([&](const bytes::Bytes&) { events.Log(LogLevel::kInfo, "Timing parameters ok"); });
}

Status UnlockSecurity(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                      IEventSink& events, const SubaruDensoSh705xKlinePlan& plan)
{
    events.Log(LogLevel::kInfo, "Requesting seed");
    return SsmStep(transport, clock, cancellation, events, plan, bytes::Bytes{0x27, 0x01}, "seed request", 10, 0x67,
                   0x01)
        .and_then(
            [&](const bytes::Bytes& seed_reply)
            {
                events.Log(LogLevel::kInfo, "Seed request ok");
                // Seed at(6..9); ECUTEK transformation for "_ecutek" methods.
                const bytes::ByteView seed = bytes::ByteView(seed_reply).subspan(6, 4);
                events.Log(LogLevel::kInfo, std::format("Received seed: {}", bytes::ToHex(seed)));
                const bytes::Bytes key = plan.seed_key == SubaruDensoSh705xKlineSeedKey::kEcuTek
                                             ? DensoSh705xKlineEcutekSeedKey(seed)
                                             : DensoSh705xKlineStockSeedKey(seed);
                events.Log(LogLevel::kInfo, std::format("Calculated seed key: {}", bytes::ToHex(key)));
                events.Log(LogLevel::kInfo, "Sending seed key to ECU");
                return SsmStep(transport, clock, cancellation, events, plan, ComposeBe(0x27_b, 0x02_b, key), "seed key",
                               6, 0x67, 0x02);
            })
        .transform([&](const bytes::Bytes&) { events.Log(LogLevel::kInfo, "Seed key ok"); });
}

// Returns the ECU ID hex.
Result<std::string> ConnectBootloader(IKlineFlashTransport& transport, IClock& clock,
                                      const ICancellationToken& cancellation, IEventSink& events,
                                      const SubaruDensoSh705xKlinePlan& plan)
{
    events.Log(LogLevel::kInfo, "No response from kernel, initialising ECU...");
    // Legacy ignored this result; a failed baud change stops here.
    Result<std::string> ecu_id =
        ChangeBaud(transport, cancellation, kSsmBaud)
            .and_then([&] { return SleepFor(clock, cancellation, kBaudSettle); })
            .and_then([&] { return RequestEcuId(transport, clock, cancellation, events, plan); });
    if (!ecu_id.has_value())
    {
        return ecu_id;
    }
    return StartCommunication(transport, clock, cancellation, events, plan)
        .and_then([&] { return UnlockSecurity(transport, clock, cancellation, events, plan); })
        .and_then(LogThen(events, "Set session mode",
                          [&]
                          {
                              return SsmStep(transport, clock, cancellation, events, plan,
                                             bytes::Bytes{0x10, 0x85, 0x02}, "session mode", 5, 0x50);
                          }))
        .transform(
            [&](const bytes::Bytes&)
            {
                events.Log(LogLevel::kInfo, "Succesfully set to programming session");
                return std::move(*ecu_id);
            });
}

// 0x80-byte blocks at addr +
// blockno * 0x80, the last carrying the rest; serial_read_timeout per block.
// A failed block logs "Write data failed!" before
// upload_kernel()'s own reply check logs why.
Status TransferKernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                      IEventSink& events, const SubaruDensoSh705xKlinePlan& plan, std::uint32_t address,
                      bytes::ByteView encrypted)
{
    const std::uint32_t length = static_cast<std::uint32_t>(encrypted.size()) & ~std::uint32_t{3};
    events.Progress(0, static_cast<int>(length));
    for (std::uint32_t offset = 0; offset < length; offset += kUploadChunkBytes)
    {
        const std::uint32_t chunk = std::min(kUploadChunkBytes, length - offset);
        Result<bytes::Bytes> reply = RequiredExchange(
            transport, clock, cancellation,
            ssm_protocol::AddHeader(ComposeBe(0x36_b, U24{address + offset}, encrypted.subspan(offset, chunk)),
                                    plan.tester_id, plan.target_id),
            0ms, kReadTimeout, "kernel transfer");
        if (!reply.has_value())
        {
            if (reply.error().kind == ErrorKind::kTimeout)
            {
                events.Log(LogLevel::kError, "Write data failed!");
                events.Log(LogLevel::kError, "No valid response from ECU");
            }
            return std::unexpected(reply.error());
        }
        if (reply->size() <= 4 || (*reply)[4] != 0x76)
        {
            events.Log(LogLevel::kError, "Write data failed!");
            return RequireSsm(*reply, 5, 0x76, std::nullopt, events);
        }
        events.Progress(static_cast<int>(offset + chunk), static_cast<int>(length));
    }
    return {};
}

// The kernel ID that proves the upload started.
Status RequireUploadedKernelId(IEventSink& events, const OptionalBytes& id)
{
    if (!id.has_value())
    {
        events.Log(LogLevel::kError, "No valid response from ECU");
        return Fail(ErrorKind::kTimeout, "no kernel ID after upload");
    }
    const bytes::Bytes& reply = *id;
    if (reply.size() <= 4)
    {
        events.Log(LogLevel::kError, "No valid response from ECU");
        return Fail(ErrorKind::kBadResponse, "kernel ID reply too short after upload");
    }
    if (!KernelReplyOk(reply, kOpId, 5))
    {
        LogWrongResponse(events, reply, 8);
        return Fail(ErrorKind::kBadResponse, "kernel did not start after upload");
    }
    events.Log(LogLevel::kInfo, std::format("Kernel ID: {}", KernelIdText(reply)));
    return {};
}

Status UploadKernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                    IEventSink& events, const SubaruDensoSh705xKlinePlan& plan, const KernelImage& kernel)
{
    events.Log(LogLevel::kDebug, std::format("Start address to upload kernel: 0x{:x}", kernel.load_address));
    // Balance, then encrypt_payload(pl_encr, pl_encr.length()).
    const bytes::Bytes balanced = DensoSh705xKlineBalancedKernel(kernel.bytes);
    const bytes::Bytes encrypted = DensoSh705xKlineEncryptPayload(balanced);
    const auto size = static_cast<std::uint32_t>(encrypted.size());

    // The one baud change legacy checked.
    return ChangeBaud(transport, cancellation, kUploadBaud)
        .and_then(LogThen(events, "Requesting kernel upload",
                          [&]
                          {
                              return SsmExchange(transport, clock, cancellation, events, plan,
                                                 ComposeBe(0x34_b, U24{kernel.load_address}, 0x04_b, U24{size}),
                                                 kExtraLongTimeout, "upload request");
                          }))
        .and_then([&](const bytes::Bytes& request) { return RequireSsm(request, 5, 0x74, std::nullopt, events); })
        .and_then(LogThen(
            events, "Kernel upload request ok", "Transfer kernel data", [&]
            { return TransferKernel(transport, clock, cancellation, events, plan, kernel.load_address, encrypted); }))
        .and_then(LogThen(events, "Kernel uploaded", "Jump to kernel",
                          [&]
                          {
                              return SsmExchange(transport, clock, cancellation, events, plan,
                                                 bytes::Bytes{0x31, 0x01, 0x01}, kExtraLongTimeout, "start routine");
                          }))
        .and_then([&](const bytes::Bytes& routine) { return RequireSsm(routine, 5, 0x71, std::nullopt, events); })
        .and_then(LogThen(events, "Kernel started, initializing...",
                          [&] { return SleepFor(clock, cancellation, kPostStartRoutine); }))
        // Correction: legacy ignored this change_port_speed("62500") result.
        .and_then([&] { return ChangeBaud(transport, cancellation, kProbeBaud); })
        .and_then(
            LogThen(events, "Requesting kernel ID", [&] { return RequestKernelId(transport, clock, cancellation); }))
        .and_then([&](const OptionalBytes& id) { return RequireUploadedKernelId(events, id); });
}

// Probe, then handshake and upload unless the kernel is
// already alive. Returns the ECU ID only when the handshake ran (legacy
// Set RomId from it on a read).
Result<std::optional<std::string>> StartSession(IKlineFlashTransport& transport, IClock& clock,
                                                const ICancellationToken& cancellation, IEventSink& events,
                                                const FlashPlan& plan)
{
    const auto& family = std::get<SubaruDensoSh705xKlinePlan>(plan.FamilyPlan());
    events.Log(LogLevel::kInfo, "Connecting to Subaru 04 32-bit K-Line bootloader, please wait...");
    Result<bool> alive = ProbeKernel(transport, clock, cancellation, events);
    if (!alive.has_value())
    {
        return std::unexpected(alive.error());
    }
    if (*alive)
    {
        return std::optional<std::string>{};
    }
    Result<std::string> ecu_id = ConnectBootloader(transport, clock, cancellation, events, family);
    if (!ecu_id.has_value())
    {
        return std::unexpected(ecu_id.error());
    }
    events.Notice("Preparing, please wait...");
    events.Log(LogLevel::kInfo, "Initializing Subaru 04 32-bit K-Line kernel upload, please wait...");
    if (Status uploaded = UploadKernel(transport, clock, cancellation, events, family, plan.KernelOrEmpty());
        !uploaded.has_value())
    {
        return std::unexpected(uploaded.error());
    }
    return std::optional<std::string>{std::move(*ecu_id)};
}

// 24-bit page address, 0x400-byte pages, 3000ms, no settle.
Result<bytes::Bytes> ReadMem(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                             IEventSink& events, const MemoryRegion& region)
{
    bytes::Bytes rom;
    rom.reserve(region.length);
    events.Log(LogLevel::kInfo, "Start reading ROM, please wait...");
    events.Progress(0, static_cast<int>(region.length));
    for (std::uint32_t address = region.start; address < region.start + region.length; address += kReadPageSize)
    {
        const bytes::Bytes request = Frame(kOpReadArea, ComposeBe(0x00_b, U24{address}, std::uint16_t(kReadPageSize)));
        Result<bytes::Bytes> reply =
            RequiredExchange(transport, clock, cancellation, request, 0ms, kExtraLongTimeout, "read");
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        // Correction: require the complete page and its sum8 before accepting it.
        const bytes::Bytes& page = *reply;
        if (page.size() != kReadPageSize + 6 || !KernelReplyOk(page, kOpReadArea, kReadPageSize + 6) ||
            page.back() != bytes::Sum8(bytes::ByteView(page).first(page.size() - 1)))
        {
            events.Log(LogLevel::kError, "Wrong response from ECU");
            return Fail(ErrorKind::kBadResponse, std::format("incomplete or corrupt page at 0x{:06X}", address));
        }
        rom.insert(rom.end(), page.begin() + 5, page.end() - 1);
        events.Progress(static_cast<int>(rom.size()), static_cast<int>(region.length));
    }
    events.Log(LogLevel::kInfo, "ROM read ready");
    rom.resize(region.length);
    return rom;
}

// ---- Write and TestWrite ------------------------------------------------

// A kernel request whose missing reply legacy logged as "No valid response
// from ECU" and stopped.
Result<bytes::Bytes> KernelExchange(IKlineFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation, IEventSink& events, bytes::ByteView request,
                                    std::chrono::milliseconds timeout, std::string_view what)
{
    Result<bytes::Bytes> reply = RequiredExchange(transport, clock, cancellation, request, 0ms, timeout, what);
    if (!reply.has_value() && reply.error().kind == ErrorKind::kTimeout)
    {
        events.Log(LogLevel::kError, "No valid response from ECU");
    }
    return reply;
}

// The legacy write-path reply check: `received.length() > min_size - 1`, else
// "No valid response from ECU"; then at(0), at(1), at(4) against the positive
// reply, else "Wrong response from ECU" -- with the NRC from mid(offset) where
// legacy appended parse_nrc_message().
Status RequireKernelReply(const bytes::Bytes& reply, std::uint8_t opcode, std::size_t min_size, IEventSink& events,
                          std::string_view what, std::optional<std::size_t> nrc_offset = std::nullopt)
{
    if (reply.size() < min_size)
    {
        events.Log(LogLevel::kError, "No valid response from ECU");
        return Fail(ErrorKind::kBadResponse, std::format("No valid response from ECU during {}", what));
    }
    if (!KernelReplyOk(reply, opcode, min_size))
    {
        if (nrc_offset.has_value())
        {
            LogWrongResponse(events, reply, *nrc_offset);
        }
        else
        {
            events.Log(LogLevel::kError, "Wrong response from ECU");
        }
        return Fail(ErrorKind::kBadResponse, std::format("Wrong response from ECU during {}", what));
    }
    return {};
}

// CRC [addr32, 00, len24], serial_read_extra_long_timeout.
// A 200ms flush read after either compare outcome (not after a
// rejected reply, which returns first).
Result<std::uint32_t> ReadBlockCrc(IKlineFlashTransport& transport, IClock& clock,
                                   const ICancellationToken& cancellation, IEventSink& events,
                                   const MemoryRegion& block)
{
    const bytes::Bytes request = Frame(kOpCrc, ComposeBe(block.start, 0x00_b, U24{block.length}));
    const Result<std::uint32_t> crc =
        KernelExchange(transport, clock, cancellation, events, request, kExtraLongTimeout, "CRC check")
            .and_then(
                [&](const bytes::Bytes& reply)
                {
                    // Correction: legacy checked `> 5` then read at(5..8); require
                    // all nine bytes before parsing.
                    return RequireKernelReply(reply, kOpCrc, 9, events, "CRC check")
                        .transform([&] { return bytes::ReadU32Be(reply, 5); });
                });
    if (!crc.has_value())
    {
        return crc;
    }
    return CheckCancelled(cancellation, "cancelled before CRC flush")
        .and_then([&] { return transport.Read(kShortTimeout, cancellation); })
        .transform([&](const OptionalBytes&) { return *crc; });
}

Result<std::vector<bool>> CompareBlocks(IKlineFlashTransport& transport, IClock& clock,
                                        const ICancellationToken& cancellation, IEventSink& events,
                                        const FlashDevice& device, bytes::ByteView image)
{
    std::vector<bool> changed(device.numblocks, false);
    for (unsigned i = 0; i < device.numblocks; ++i)
    {
        if (Status cancelled = CheckCancelled(cancellation, "cancelled during ROM compare"); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        const MemoryRegion block{device.fblocks[i].start, device.fblocks[i].len};
        events.Log(LogLevel::kInfo, std::format("FB{:02}\t0x{:08X}\t0x{:08X}", i, block.start, block.length));
        Result<std::uint32_t> ecu_crc = ReadBlockCrc(transport, clock, cancellation, events, block);
        if (!ecu_crc.has_value())
        {
            return std::unexpected(ecu_crc.error());
        }
        const std::uint32_t image_crc = fastecu::checksum::Crc32(image.subspan(block.start, block.length));
        changed[i] = *ecu_crc != image_crc;
        events.Log(LogLevel::kDebug, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", *ecu_crc, image_crc));
        events.Log(LogLevel::kInfo, std::format("\t{:08X}\t{:08X}", *ecu_crc, image_crc));
        events.Log(LogLevel::kInfo, changed[i] ? "\tNO" : "\tYES");
        if (Status slept = SleepFor(clock, cancellation, kCrcBlockPacing); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    return changed;
}

unsigned LogChangedBlocks(IEventSink& events, const std::vector<bool>& changed)
{
    unsigned count = 0;
    events.Log(LogLevel::kInfo, "Different blocks : ");
    for (std::size_t i = 0; i < changed.size(); ++i)
    {
        if (changed[i])
        {
            events.Log(LogLevel::kInfo, std::format("{}, ", i));
            ++count;
        }
    }
    events.Log(LogLevel::kInfo, std::format(" (total: {})", count));
    return count;
}

// The sizes are queried and logged only; legacy
// flash_block() hard-codes 0x200/0x1000. The mode ack gates everything after.
Status InitFlashWrite(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                      IEventSink& events, bool test_write)
{
    for (const std::uint8_t opcode : {kOpGetMaxMsgSize, kOpGetMaxBlockSize})
    {
        events.Log(LogLevel::kInfo, opcode == kOpGetMaxMsgSize ? "Check max message length" : "Check flashblock size");
        Result<bytes::Bytes> reply =
            KernelExchange(transport, clock, cancellation, events, Frame(opcode), kMediumTimeout, "flash init");
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        // `> 9`, value at(5..8).
        if (Status ok = RequireKernelReply(*reply, opcode, 10, events, "flash init"); !ok.has_value())
        {
            return ok;
        }
        events.Log(LogLevel::kInfo, std::format(": 0x{:04x}", bytes::ReadU32Be(*reply, 5)));
    }
    const std::uint8_t mode = test_write ? kOpFlashDisable : kOpFlashEnable;
    events.Log(LogLevel::kInfo, test_write ? "Test write mode on, no actual flash write is performed"
                                           : "Test write mode off, perform actual flash write");
    return KernelExchange(transport, clock, cancellation, events, Frame(mode), kMediumTimeout, "flash mode")
        // `> 5`; a rejected mode stops the write (legacy returned
        // STATUS_ERROR, and write_mem() stopped before any erase).
        .and_then([&](const bytes::Bytes& reply) { return RequireKernelReply(reply, mode, 6, events, "flash mode"); })
        .transform([&] { events.Log(LogLevel::kError, "Flash mode succesfully set"); }); // Legacy LOG_E
}

std::uint64_t ElapsedMilliseconds(std::chrono::steady_clock::time_point start,
                                  std::chrono::steady_clock::time_point end)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 1U;
}

// BLANK_PAGE is sent in both modes, as in legacy;
// then 0x200-byte WRITE_FLASH_BUFFER chunks, and at each 0x1000 boundary a
// COMMIT (write) or VALIDATE (test write) carrying the image CRC32.
Status FlashBlock(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                  IEventSink& events, bytes::ByteView image, const MemoryRegion& block, bool test_write,
                  std::uint64_t& written, std::uint64_t total)
{
    events.Log(LogLevel::kInfo,
               std::format("Flash page erase addr: 0x{:08x} len: 0x{:08x}", block.start, block.length));
    events.Log(LogLevel::kInfo, "Erasing flash page...");
    if (Status erased = KernelExchange(transport, clock, cancellation, events,
                                       Frame(kOpBlankPage, ComposeBe(block.start)), kExtraLongTimeout, "erase")
                            .and_then([&](const bytes::Bytes& reply)
                                      { return RequireKernelReply(reply, kOpBlankPage, 5, events, "erase", 8); });
        !erased.has_value())
    {
        return erased;
    }
    events.Log(LogLevel::kInfo, " erased");
    events.Log(LogLevel::kInfo,
               std::format("Start flash write addr: 0x{:08x} len: 0x{:08x}", block.start, block.length));

    std::uint32_t commit_start = block.start;
    for (std::uint32_t offset = 0; offset < block.length; offset += kWriteChunkSize)
    {
        const auto chunk_started = clock.Now();
        if (Status cancelled = CheckCancelled(cancellation, "cancelled during flash write"); !cancelled.has_value())
        {
            return cancelled;
        }
        // `> 5`.
        const std::uint32_t address = block.start + offset;
        Result<bytes::Bytes> chunk =
            KernelExchange(transport, clock, cancellation, events,
                           Frame(kOpWriteFlashBuffer, ComposeBe(address, image.subspan(address, kWriteChunkSize))),
                           kExtraLongTimeout, "write");
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        if (Status ok = RequireKernelReply(*chunk, kOpWriteFlashBuffer, 6, events, "write"); !ok.has_value())
        {
            return ok;
        }
        events.Log(LogLevel::kDebug, "Data written to flash buffer");
        // Legacy printed the previous chunk's speed (and an
        // uninitialised one first); this chunk's is printed instead.
        const std::uint64_t elapsed_ms = ElapsedMilliseconds(chunk_started, clock.Now());
        written += kWriteChunkSize;
        const TransferRate rate = ComputeTransferRate(kWriteChunkSize, elapsed_ms, total - written);
        events.Log(LogLevel::kInfo, FormatWriteProgress(address, (100U * offset) / block.length, rate));
        events.Progress(static_cast<int>(written), static_cast<int>(total));

        if (commit_start + kCommitBlockSize == address + kWriteChunkSize)
        {
            const std::uint32_t crc = fastecu::checksum::Crc32(image.subspan(commit_start, kCommitBlockSize));
            events.Log(LogLevel::kInfo, "Flash buffer write complete... ");
            events.Log(LogLevel::kDebug, std::format("Image CRC32: 0x{:x}", crc));
            const std::uint8_t commit = test_write ? kOpValidateFlashBuffer : kOpCommitFlashBuffer;
            events.Log(LogLevel::kInfo, test_write ? std::format("Validate flash addr: 0x{:x}", commit_start)
                                                   : std::format("Committ flash addr: 0x{:x}", commit_start));
            events.Log(LogLevel::kInfo, std::format(" len: 0x{:x}", kCommitBlockSize));
            events.Log(LogLevel::kInfo, std::format(" crc32: 0x{:x}", crc));
            Result<bytes::Bytes> committed = KernelExchange(
                transport, clock, cancellation, events,
                Frame(commit, ComposeBe(commit_start, static_cast<std::uint16_t>(kCommitBlockSize), crc)),
                kExtraLongTimeout, "commit");
            if (!committed.has_value())
            {
                return std::unexpected(committed.error());
            }
            if (Status ok = RequireKernelReply(*committed, commit, 6, events, "commit"); !ok.has_value())
            {
                return ok;
            }
            commit_start += kCommitBlockSize;
        }
    }
    return {};
}

// Init on the first block, then PROG_VOLT and
// flash_block().
Status ReflashBlock(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                    IEventSink& events, bytes::ByteView image, const MemoryRegion& block, bool test_write,
                    bool& flash_write_init, std::uint64_t& written, std::uint64_t total)
{
    if (!flash_write_init)
    {
        if (Status ok = InitFlashWrite(transport, clock, cancellation, events, test_write); !ok.has_value())
        {
            return ok;
        }
        flash_write_init = true;
    }
    events.Log(LogLevel::kInfo, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    // `> 9`, voltage at(5..6) / 50.
    events.Log(LogLevel::kInfo, "Check flash voltage");
    const Result<std::uint16_t> volt =
        KernelExchange(transport, clock, cancellation, events, Frame(kOpProgVolt), kMediumTimeout, "prog voltage")
            .and_then(
                [&](const bytes::Bytes& reply)
                {
                    return RequireKernelReply(reply, kOpProgVolt, 10, events, "prog voltage")
                        .transform([&] { return bytes::ReadU16Be(reply, 5); });
                });
    if (!volt.has_value())
    {
        return std::unexpected(volt.error());
    }
    // QString::number(float) is 'g' with precision 6, as is std::format's {:g}.
    const auto prog_voltage = static_cast<float>(*volt / 50.0);
    events.Log(LogLevel::kInfo, std::format(": {:g}V", prog_voltage));

    if (Status flashed = FlashBlock(transport, clock, cancellation, events, image, block, test_write, written, total);
        !flashed.has_value())
    {
        events.Log(LogLevel::kError, "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is "
                                     "most likely still running and receiving commands!");
        return flashed;
    }
    events.Log(LogLevel::kInfo, "Flash block ok");
    return {};
}

Status WriteMem(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                IEventSink& events, const FlashPlan& plan)
{
    const FlashDevice *device = FindFlashDevice(plan.McuName());
    if (device == nullptr || !plan.Image().has_value() || plan.Image()->size() < device->romsize)
    {
        return Fail(ErrorKind::kInvalidConfig, "Denso SH705x K-Line write needs the full ROM image");
    }
    // Legacy indexed the image by absolute flash address (fblocks[0].start is
    // 0 for every SH705x device), for the compare and the write alike.
    const bytes::ByteView image = *plan.Image();
    const bool test_write = plan.Operation() == FlashOperation::kTestWrite;

    events.Log(LogLevel::kInfo, "--- Comparing ECU flash memory pages to image file ---");
    events.Log(LogLevel::kInfo, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    Result<std::vector<bool>> changed = CompareBlocks(transport, clock, cancellation, events, *device, image);
    if (!changed.has_value())
    {
        events.Log(LogLevel::kError, "Error in ROM compare");
        return std::unexpected(changed.error());
    }
    if (LogChangedBlocks(events, *changed) == 0)
    {
        events.Log(LogLevel::kInfo,
                   "*** Compare results no difference between ROM and ECU data, no flashing needed! ***");
        return {};
    }

    std::uint64_t total = 0;
    for (unsigned i = 0; i < device->numblocks; ++i)
    {
        if ((*changed)[i])
        {
            total += device->fblocks[i].len;
        }
    }
    events.Log(LogLevel::kInfo, "--- Start writing ROM file to ECU flash memory ---");
    events.Progress(0, static_cast<int>(total));
    bool flash_write_init = false;
    std::uint64_t written = 0;
    for (unsigned i = 0; i < device->numblocks; ++i)
    {
        if (!(*changed)[i])
        {
            continue;
        }
        const MemoryRegion block{device->fblocks[i].start, device->fblocks[i].len};
        if (Status ok = ReflashBlock(transport, clock, cancellation, events, image, block, test_write, flash_write_init,
                                     written, total);
            !ok.has_value())
        {
            events.Log(LogLevel::kInfo, std::format("Block {} reflash failed.", i));
            return ok;
        }
        events.Log(LogLevel::kInfo, std::format("Block {} reflash complete.", i));
    }

    events.Log(LogLevel::kInfo, "--- Comparing ECU flash memory pages to image file after reflash ---");
    events.Log(LogLevel::kInfo, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    Result<std::vector<bool>> after = CompareBlocks(transport, clock, cancellation, events, *device, image);
    if (!after.has_value())
    {
        events.Log(LogLevel::kError, "Error in ROM compare");
        return std::unexpected(after.error());
    }
    const unsigned still_different = LogChangedBlocks(events, *after);
    if (test_write)
    {
        // A validate-only pass leaves the flash unchanged, so the
        // post-compare still differs by design.
        events.Log(LogLevel::kInfo, "*** Test write PASS, it's ok to perform actual write! ***");
        return {};
    }
    if (still_different != 0)
    {
        events.Log(LogLevel::kError, "*** ERROR IN FLASH PROCESS ***");
        events.Log(LogLevel::kError,
                   "Don't power off your ECU, kernel is still running and you can try flashing again!");
        // Correction: legacy returned STATUS_SUCCESS here.
        return Fail(ErrorKind::kBadResponse, "ECU flash still differs from the image after reflash");
    }
    return {};
}

} // namespace

bytes::Bytes DensoSh705xKlineBalancedKernel(bytes::ByteView kernel)
{
    bytes::Bytes out(kernel.begin(), kernel.end());
    out.push_back(0x00);
    out.push_back(0x00);
    out.resize((out.size() + 3) & ~std::size_t{3}, 0x00);
    out.resize(out.size() - 2);
    const auto at = [&out](std::size_t index) -> std::uint32_t
    { return index < out.size() ? static_cast<std::uint32_t>(out[index]) : 0U; };
    std::uint32_t sum = 0U;
    for (std::size_t i = 0; i < out.size(); i += 4)
    {
        sum += (at(i) << 24U) | (at(i + 1) << 16U) | (at(i + 2) << 8U) | at(i + 3);
    }
    const std::uint32_t balance = (0x5AA5U - sum) & 0xFFFFU;
    out.push_back(static_cast<bytes::Byte>(balance >> 8U));
    out.push_back(static_cast<bytes::Byte>(balance & 0xFFU));
    return out;
}

Result<KlineConfig> SubaruDensoSh705xKlineExecutor::Setup(const SubaruDensoSh705xKlinePlan& family) const
{
    return NonIso14230KlineConfigFrom(family);
}

Status SubaruDensoSh705xKlineExecutor::BeforeTransportConfigure(IKlineFlashTransport& transport, IClock&,
                                                                const ICancellationToken& cancellation) const
{
    // serial->reset_connection() before every setter.
    return CheckCancelled(cancellation, "cancelled before K-Line reset")
        .and_then([&] { return transport.ResetConnection(); })
        .and_then([&] { return CheckCancelled(cancellation, "cancelled after K-Line reset"); });
}

Result<FlashExecutionResult> SubaruDensoSh705xKlineExecutor::Run(const SubaruDensoSh705xKlinePlan& /*family*/,
                                                                 const FlashPlan& plan, IKlineFlashTransport& transport,
                                                                 IClock& clock, const ICancellationToken& cancellation,
                                                                 IEventSink& events)
{
    // The shared serial can arrive with the header flag set by
    // another feature; every BEEF and SSM frame of this family goes out bare.
    Result<std::optional<std::string>> ecu_id = transport.SetAddIso14230Header(false).and_then(
        [&] { return StartSession(transport, clock, cancellation, events, plan); });
    if (!ecu_id.has_value())
    {
        return std::unexpected(ecu_id.error());
    }
    if (plan.Operation() == FlashOperation::kRead)
    {
        // The notice precedes the log line, as in every sibling executor.
        events.Notice("Reading ROM, please wait...");
        events.Log(LogLevel::kInfo, "Reading ROM from Subaru 04 32-bit using K-Line");
        Result<bytes::Bytes> rom = ReadMem(transport, clock, cancellation, events, plan.TransferRegion());
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        std::optional<std::string> rom_id;
        if (ecu_id->has_value())
        {
            rom_id = **ecu_id + "_"; // connect_bootloader(): RomId = ecuid + "_"
        }
        return FlashExecutionResult{.operation = plan.Operation(), .read_bytes = std::move(*rom), .rom_id = rom_id};
    }
    // test_write and write share write_mem().
    events.Notice("Writing ROM, please wait...");
    events.Log(LogLevel::kInfo, "Writing ROM to Subaru 04 32-bit using K-Line");
    if (Status written = WriteMem(transport, clock, cancellation, events, plan); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{.operation = plan.Operation(), .read_bytes = std::nullopt};
}

} // namespace fastecu::flash
