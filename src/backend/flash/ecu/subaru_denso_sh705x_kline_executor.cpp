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
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/denso_sh705x_kline_common.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::composeBeWithChecksum;
using bytes::u24;
using namespace bytes::literals;
using namespace std::chrono_literals;
using OptionalBytes = IKlineFlashTransport::OptionalBytes;

// Legacy: src/platform/desktop/common/flash/legacy/ecu/
// flash_ecu_subaru_denso_sh705x_kline_operation.{h,cpp}, deleted in wave 6b-2.
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

// Header :48-53.
constexpr std::chrono::milliseconds kReadTimeout = 2000ms;      // serial_read_timeout
constexpr std::chrono::milliseconds kShortTimeout = 200ms;      // serial_read_short_timeout
constexpr std::chrono::milliseconds kMediumTimeout = 500ms;     // serial_read_medium_timeout
constexpr std::chrono::milliseconds kLongTimeout = 800ms;       // serial_read_long_timeout
constexpr std::chrono::milliseconds kExtraLongTimeout = 3000ms; // serial_read_extra_long_timeout

constexpr int kProbeBaud = 62500;                              // connect_bootloader():127, upload_kernel():463
constexpr int kSsmBaud = 4800;                                 // connect_bootloader():161
constexpr int kUploadBaud = 15625;                             // upload_kernel():354
constexpr std::chrono::milliseconds kBaudSettle = 100ms;       // connect_bootloader():128, :162
constexpr std::chrono::milliseconds kKernelIdSettle = 200ms;   // request_kernel_id() delay(200)
constexpr std::chrono::milliseconds kPostStartRoutine = 100ms; // upload_kernel() delay(100) before 62500
constexpr std::chrono::milliseconds kCrcBlockPacing = 5ms;     // get_changed_blocks() delay(5)

constexpr std::uint32_t kReadPageSize = 0x400;     // read_mem() pagesize
constexpr std::uint32_t kWriteChunkSize = 0x200;   // flash_block() blocksize
constexpr std::uint32_t kCommitBlockSize = 0x1000; // flash_block() flashblocksize
constexpr std::uint32_t kUploadChunkBytes = 0x80;  // send_sid_36_transferdata() blocksize

Status check_cancelled(const ICancellationToken& cancellation, std::string detail)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::move(detail));
    }
    return {};
}

// Every kernel request, e.g. request_kernel_id() and check_romcrc():
// BE EF, u16 length (opcode + payload), opcode, payload, sum8.
bytes::Bytes frame(std::uint8_t opcode, bytes::ByteView payload = {})
{
    return composeBeWithChecksum(bytes::sum8, kStartComm, static_cast<std::uint16_t>(payload.size() + 1),
                                 bytes::Byte(opcode), payload);
}

// Legacy kernel-reply checks index at(0), at(1) and at(4) after a length check.
bool kernel_reply_ok(bytes::ByteView received, std::uint8_t opcode, std::size_t min_size)
{
    return received.size() >= min_size && received[0] == 0xBE && received[1] == 0xEF &&
           received[4] == static_cast<bytes::Byte>(opcode | 0x40U);
}

// write -> [settle] -> read, cancellation-checked at every boundary.
Result<OptionalBytes> exchange(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                               bytes::ByteView request, std::chrono::milliseconds settle,
                               std::chrono::milliseconds timeout)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before write"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    Result<std::size_t> written = transport.write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return fail(ErrorKind::Disconnected, "short K-Line write");
    }
    if (settle > 0ms)
    {
        if (Status slept = clock.sleep(settle, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before read"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    Result<OptionalBytes> received = transport.read(timeout, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after read"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    return received;
}

// Like exchange(), but no frame at all is a Timeout -- legacy treated an
// empty reply as "No valid response from ECU" and stopped.
Result<bytes::Bytes> required_exchange(IKlineFlashTransport& transport, IClock& clock,
                                       const ICancellationToken& cancellation, bytes::ByteView request,
                                       std::chrono::milliseconds settle, std::chrono::milliseconds timeout,
                                       std::string_view what)
{
    Result<OptionalBytes> received = exchange(transport, clock, cancellation, request, settle, timeout);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, std::format("no response from ECU during {}", what));
    }
    return std::move(**received);
}

Status change_baud(IKlineFlashTransport& transport, const ICancellationToken& cancellation, int baud)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before changing baud"); !cancelled.has_value())
    {
        return cancelled;
    }
    return transport.setBaud(baud);
}

Status sleep_for(IClock& clock, const ICancellationToken& cancellation, std::chrono::milliseconds duration)
{
    return clock.sleep(duration, cancellation);
}

// Legacy: emit LOG_E("Wrong response from ECU: " +
// FileActions::parse_nrc_message(received.mid(offset, ...))), which is
// nrc_description() over everything from `offset` on (empty past the end).
void log_wrong_response(IEventSink& events, bytes::ByteView received, std::size_t offset)
{
    const bytes::ByteView nrc = offset < received.size() ? received.subspan(offset) : bytes::ByteView{};
    events.log(LogLevel::Error, "Wrong response from ECU: " + nrc_description(nrc));
}

// received.remove(0, 5); received.remove(received.length() - 1, 1): the
// kernel ID text between the opcode byte and the checksum. A five-byte reply
// passes legacy's `> 4` check and leaves an empty ID.
std::string kernel_id_text(bytes::ByteView reply)
{
    return reply.size() > 5 ? std::string(reply.begin() + 5, reply.end() - 1) : std::string{};
}

// connect_bootloader():186-189 -- each ECU ID byte as two uppercase hex digits.
std::string ecu_id_hex(bytes::ByteView id)
{
    std::string out;
    for (const bytes::Byte byte : id)
    {
        out += std::format("{:02X}", byte);
    }
    return out;
}

// request_kernel_id():1700-1713 -- BE EF 00 01 01 sum8, delay(200),
// read_serial_data(serial_read_long_timeout).
Result<OptionalBytes> request_kernel_id(IKlineFlashTransport& transport, IClock& clock,
                                        const ICancellationToken& cancellation)
{
    return exchange(transport, clock, cancellation, frame(kOpId), kKernelIdSettle, kLongTimeout);
}

// connect_bootloader():127-157. A live kernel is the only success; any other
// reply is the normal "not running" case and falls through, as in legacy.
Result<bool> probe_kernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                          IEventSink& events)
{
    // connect_bootloader():127-128. Legacy ignored this result; a failed baud
    // change stops here (see the Task 6 report).
    if (Status baud = change_baud(transport, cancellation, kProbeBaud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    if (Status slept = sleep_for(clock, cancellation, kBaudSettle); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    events.log(LogLevel::Info, "Checking if kernel is already running...");             // :130
    events.log(LogLevel::Info, "Requesting kernel ID");                                 // :131
    Result<OptionalBytes> received = request_kernel_id(transport, clock, cancellation); // :133
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value() || (**received).size() <= 4) // :134
    {
        events.log(LogLevel::Error, "No valid response from ECU"); // :156
        return false;
    }
    const bytes::Bytes& reply = **received;
    if (!kernel_reply_ok(reply, kOpId, 5)) // :136-138
    {
        log_wrong_response(events, reply, 8); // :140-142, mid(8)
        return false;
    }
    events.log(LogLevel::Info, "Kernel ID: " + kernel_id_text(reply)); // :146-148
    return true;
}

// Every send_sid_*() helper: SsmProtocol::addHeader(payload, tester_id,
// target_id), write_serial_data_echo_check(), read_serial_data(timeout); no
// delay. Legacy then logged "No valid response from ECU" when nothing came
// back (the `> N` length check failing on an empty reply).
Result<bytes::Bytes> ssm_exchange(IKlineFlashTransport& transport, IClock& clock,
                                  const ICancellationToken& cancellation, IEventSink& events,
                                  const SubaruDensoSh705xKlinePlan& plan, bytes::ByteView payload,
                                  std::chrono::milliseconds timeout, std::string_view what)
{
    Result<bytes::Bytes> reply =
        required_exchange(transport, clock, cancellation,
                          SsmProtocol::addHeader(payload, plan.tester_id, plan.target_id), 0ms, timeout, what);
    if (!reply.has_value() && reply.error().kind == ErrorKind::Timeout)
    {
        events.log(LogLevel::Error, "No valid response from ECU");
    }
    return reply;
}

// The legacy reply check shared by every SSM step: `received.length() >
// min_size - 1`, else "No valid response from ECU"; then at(4) (and at(5))
// against the positive response, else "Wrong response from ECU: " + NRC.
Status require_ssm(const bytes::Bytes& reply, std::size_t min_size, std::uint8_t byte4,
                   std::optional<std::uint8_t> byte5, IEventSink& events)
{
    if (reply.size() < min_size)
    {
        events.log(LogLevel::Error, "No valid response from ECU");
        return fail(ErrorKind::BadResponse, "No valid response from ECU");
    }
    if (reply[4] != byte4 || (byte5.has_value() && reply[5] != *byte5))
    {
        log_wrong_response(events, reply, 4);
        return fail(ErrorKind::BadResponse, "Wrong response from ECU");
    }
    return {};
}

// connect_bootloader():159-323. Returns the ECU ID hex (legacy :184-192).
Result<std::string> connect_bootloader(IKlineFlashTransport& transport, IClock& clock,
                                       const ICancellationToken& cancellation, IEventSink& events,
                                       const SubaruDensoSh705xKlinePlan& plan)
{
    events.log(LogLevel::Info, "No response from kernel, initialising ECU..."); // :159
    // :161-162. Legacy ignored this result; a failed baud change stops here.
    if (Status baud = change_baud(transport, cancellation, kSsmBaud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    if (Status slept = sleep_for(clock, cancellation, kBaudSettle); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }

    // :164-182, send_sid_bf_ssm_init():1373-1378.
    events.log(LogLevel::Info, "Requesting ECU ID");
    Result<bytes::Bytes> bf =
        ssm_exchange(transport, clock, cancellation, events, plan, bytes::Bytes{0xBF}, kReadTimeout, "ECU ID request");
    if (!bf.has_value())
    {
        return std::unexpected(bf.error());
    }
    if (Status ok = require_ssm(*bf, 5, 0xFF, std::nullopt, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    // Correction: legacy checked only `> 4`, then removed 8 bytes and kept 5
    // (:184-185); require all 13 before slicing. The positive-reply check
    // above stays first so a negative reply still logs its NRC.
    if (bf->size() < 13)
    {
        events.log(LogLevel::Error, "No valid response from ECU");
        return fail(ErrorKind::BadResponse, "ECU ID reply too short");
    }
    std::string ecu_id = ecu_id_hex(bytes::ByteView(*bf).subspan(8, 5));
    events.log(LogLevel::Info, "ECU ID: " + ecu_id); // :192

    // :198-217, send_sid_81_start_communication():1393-1398.
    events.log(LogLevel::Info, "Requesting to start communication");
    Result<bytes::Bytes> start = ssm_exchange(transport, clock, cancellation, events, plan, bytes::Bytes{0x81},
                                              kReadTimeout, "start communication");
    if (!start.has_value())
    {
        return std::unexpected(start.error());
    }
    if (Status ok = require_ssm(*start, 5, 0xC1, std::nullopt, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    events.log(LogLevel::Info, "Start communication ok");

    // :219-238, send_sid_83_request_timings():1413-1419.
    events.log(LogLevel::Info, "Requesting timings params");
    Result<bytes::Bytes> timings = ssm_exchange(transport, clock, cancellation, events, plan, bytes::Bytes{0x83, 0x00},
                                                kReadTimeout, "timing parameters");
    if (!timings.has_value())
    {
        return std::unexpected(timings.error());
    }
    if (Status ok = require_ssm(*timings, 5, 0xC3, std::nullopt, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    events.log(LogLevel::Info, "Timing parameters ok");

    // :240-259, send_sid_27_request_seed():1434-1440.
    events.log(LogLevel::Info, "Requesting seed");
    Result<bytes::Bytes> seed_reply = ssm_exchange(transport, clock, cancellation, events, plan,
                                                   bytes::Bytes{0x27, 0x01}, kReadTimeout, "seed request");
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    if (Status ok = require_ssm(*seed_reply, 10, 0x67, 0x01, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    events.log(LogLevel::Info, "Seed request ok");

    // :261-279 -- seed at(6..9); ECUTEK transformation for "_ecutek" methods.
    const bytes::ByteView seed = bytes::ByteView(*seed_reply).subspan(6, 4);
    events.log(LogLevel::Info, "Received seed: " + bytes::toHex(seed));
    const bytes::Bytes key = plan.seed_key == SubaruDensoSh705xKlineSeedKey::EcuTek
                                 ? denso_sh705x_kline_ecutek_seed_key(seed)
                                 : denso_sh705x_kline_stock_seed_key(seed);
    events.log(LogLevel::Info, "Calculated seed key: " + bytes::toHex(key));

    // :281-300, send_sid_27_send_seed_key():1455-1462.
    events.log(LogLevel::Info, "Sending seed key to ECU");
    Result<bytes::Bytes> key_reply = ssm_exchange(transport, clock, cancellation, events, plan,
                                                  composeBe(0x27_b, 0x02_b, key), kReadTimeout, "seed key");
    if (!key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    if (Status ok = require_ssm(*key_reply, 6, 0x67, 0x02, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    events.log(LogLevel::Info, "Seed key ok");

    // :302-321, send_sid_10_start_diagnostic():1477-1484.
    events.log(LogLevel::Info, "Set session mode");
    Result<bytes::Bytes> session = ssm_exchange(transport, clock, cancellation, events, plan,
                                                bytes::Bytes{0x10, 0x85, 0x02}, kReadTimeout, "session mode");
    if (!session.has_value())
    {
        return std::unexpected(session.error());
    }
    if (Status ok = require_ssm(*session, 5, 0x50, std::nullopt, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    events.log(LogLevel::Info, "Succesfully set to programming session");
    return ecu_id;
}

// send_sid_36_transferdata():1526-1590 -- 0x80-byte blocks at addr +
// blockno * 0x80, the last carrying the rest; serial_read_timeout per block.
// A failed block logs "Write data failed!" (:1575, :1581) before
// upload_kernel()'s own reply check (:415-431) logs why.
Status transfer_kernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                       IEventSink& events, const SubaruDensoSh705xKlinePlan& plan, std::uint32_t address,
                       bytes::ByteView encrypted)
{
    const std::uint32_t length = static_cast<std::uint32_t>(encrypted.size()) & ~std::uint32_t{3};
    events.progress(0, static_cast<int>(length));
    for (std::uint32_t offset = 0; offset < length; offset += kUploadChunkBytes)
    {
        const std::uint32_t chunk = std::min(kUploadChunkBytes, length - offset);
        Result<bytes::Bytes> reply = required_exchange(
            transport, clock, cancellation,
            SsmProtocol::addHeader(composeBe(0x36_b, u24(address + offset), encrypted.subspan(offset, chunk)),
                                   plan.tester_id, plan.target_id),
            0ms, kReadTimeout, "kernel transfer");
        if (!reply.has_value())
        {
            if (reply.error().kind == ErrorKind::Timeout)
            {
                events.log(LogLevel::Error, "Write data failed!");
                events.log(LogLevel::Error, "No valid response from ECU");
            }
            return std::unexpected(reply.error());
        }
        if (reply->size() <= 4 || (*reply)[4] != 0x76)
        {
            events.log(LogLevel::Error, "Write data failed!");
            return require_ssm(*reply, 5, 0x76, std::nullopt, events);
        }
        events.progress(static_cast<int>(offset + chunk), static_cast<int>(length));
    }
    return {};
}

// upload_kernel():331-496.
Status upload_kernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                     IEventSink& events, const SubaruDensoSh705xKlinePlan& plan, const KernelImage& kernel)
{
    events.log(LogLevel::Debug, std::format("Start address to upload kernel: 0x{:x}", kernel.load_address)); // :345
    // :354-357 -- the one baud change legacy checked.
    if (Status baud = change_baud(transport, cancellation, kUploadBaud); !baud.has_value())
    {
        return baud;
    }
    // :366-389 -- balance, then encrypt_payload(pl_encr, pl_encr.length()).
    const bytes::Bytes balanced = denso_sh705x_kline_balanced_kernel(kernel.bytes);
    const bytes::Bytes encrypted =
        denso_sh705x_kline_encrypt_payload(balanced, static_cast<std::uint32_t>(balanced.size()));
    const auto size = static_cast<std::uint32_t>(encrypted.size());

    // :391-411, send_sid_34_request_upload():1494-1506.
    events.log(LogLevel::Info, "Requesting kernel upload");
    Result<bytes::Bytes> request = ssm_exchange(transport, clock, cancellation, events, plan,
                                                composeBe(0x34_b, u24(kernel.load_address), 0x04_b, u24(size)),
                                                kExtraLongTimeout, "upload request");
    if (!request.has_value())
    {
        return std::unexpected(request.error());
    }
    if (Status ok = require_ssm(*request, 5, 0x74, std::nullopt, events); !ok.has_value())
    {
        return ok;
    }
    events.log(LogLevel::Info, "Kernel upload request ok");

    // :413-432.
    events.log(LogLevel::Info, "Transfer kernel data");
    if (Status sent = transfer_kernel(transport, clock, cancellation, events, plan, kernel.load_address, encrypted);
        !sent.has_value())
    {
        return sent;
    }
    events.log(LogLevel::Info, "Kernel uploaded");

    // :434-460 -- 31 01 01, read_serial_data(serial_read_extra_long_timeout).
    events.log(LogLevel::Info, "Jump to kernel");
    Result<bytes::Bytes> routine = ssm_exchange(transport, clock, cancellation, events, plan,
                                                bytes::Bytes{0x31, 0x01, 0x01}, kExtraLongTimeout, "start routine");
    if (!routine.has_value())
    {
        return std::unexpected(routine.error());
    }
    if (Status ok = require_ssm(*routine, 5, 0x71, std::nullopt, events); !ok.has_value())
    {
        return ok;
    }
    events.log(LogLevel::Info, "Kernel started, initializing...");

    // :462-463.
    if (Status slept = sleep_for(clock, cancellation, kPostStartRoutine); !slept.has_value())
    {
        return slept;
    }
    // Correction: legacy ignored this change_port_speed("62500") result.
    if (Status baud = change_baud(transport, cancellation, kProbeBaud); !baud.has_value())
    {
        return baud;
    }

    // :465-495.
    events.log(LogLevel::Info, "Requesting kernel ID");
    Result<OptionalBytes> id = request_kernel_id(transport, clock, cancellation);
    if (!id.has_value())
    {
        return std::unexpected(id.error());
    }
    if (!id->has_value())
    {
        events.log(LogLevel::Error, "No valid response from ECU"); // :492
        return fail(ErrorKind::Timeout, "no kernel ID after upload");
    }
    const bytes::Bytes& reply = **id;
    if (reply.size() <= 4)
    {
        events.log(LogLevel::Error, "No valid response from ECU"); // :492
        return fail(ErrorKind::BadResponse, "kernel ID reply too short after upload");
    }
    if (!kernel_reply_ok(reply, kOpId, 5)) // :470-472
    {
        log_wrong_response(events, reply, 8); // :474-476, mid(8)
        return fail(ErrorKind::BadResponse, "kernel did not start after upload");
    }
    events.log(LogLevel::Info, "Kernel ID: " + kernel_id_text(reply)); // :482-484
    return {};
}

// execute():79-87 -- probe, then handshake and upload unless the kernel is
// already alive. Returns the ECU ID only when the handshake ran (legacy
// :193-196 set RomId from it on a read).
Result<std::optional<std::string>> start_session(IKlineFlashTransport& transport, IClock& clock,
                                                 const ICancellationToken& cancellation, IEventSink& events,
                                                 const FlashPlan& plan)
{
    const auto& family = std::get<SubaruDensoSh705xKlinePlan>(plan.family_plan());
    events.log(LogLevel::Info, "Connecting to Subaru 04 32-bit K-Line bootloader, please wait..."); // :79
    Result<bool> alive = probe_kernel(transport, clock, cancellation, events);
    if (!alive.has_value())
    {
        return std::unexpected(alive.error());
    }
    if (*alive)
    {
        return std::optional<std::string>{};
    }
    Result<std::string> ecu_id = connect_bootloader(transport, clock, cancellation, events, family);
    if (!ecu_id.has_value())
    {
        return std::unexpected(ecu_id.error());
    }
    events.notice("Preparing, please wait...");                                                       // :84
    events.log(LogLevel::Info, "Initializing Subaru 04 32-bit K-Line kernel upload, please wait..."); // :85
    if (Status uploaded = upload_kernel(transport, clock, cancellation, events, family, *plan.kernel());
        !uploaded.has_value())
    {
        return std::unexpected(uploaded.error());
    }
    return std::optional<std::string>{std::move(*ecu_id)};
}

// read_mem():503-640. 24-bit page address, 0x400-byte pages, 3000ms, no settle.
Result<bytes::Bytes> read_mem(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              IEventSink& events, const MemoryRegion& region)
{
    bytes::Bytes rom;
    rom.reserve(region.length);
    events.log(LogLevel::Info, "Start reading ROM, please wait..."); // :523
    events.progress(0, static_cast<int>(region.length));
    for (std::uint32_t address = region.start; address < region.start + region.length; address += kReadPageSize)
    {
        const bytes::Bytes request = frame(kOpReadArea, composeBe(0x00_b, u24(address), std::uint16_t(kReadPageSize)));
        Result<bytes::Bytes> reply =
            required_exchange(transport, clock, cancellation, request, 0ms, kExtraLongTimeout, "read");
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        // Correction: require the complete page and its sum8 before accepting it.
        const bytes::Bytes& page = *reply;
        if (page.size() != kReadPageSize + 6 || !kernel_reply_ok(page, kOpReadArea, kReadPageSize + 6) ||
            page.back() != bytes::sum8(bytes::ByteView(page).first(page.size() - 1)))
        {
            events.log(LogLevel::Error, "Wrong response from ECU");
            return fail(ErrorKind::BadResponse, std::format("incomplete or corrupt page at 0x{:06X}", address));
        }
        rom.insert(rom.end(), page.begin() + 5, page.end() - 1);
        events.progress(static_cast<int>(rom.size()), static_cast<int>(region.length));
    }
    events.log(LogLevel::Info, "ROM read ready"); // :628
    rom.resize(region.length);
    return rom;
}

// ---- Write and TestWrite ------------------------------------------------

// A kernel request whose missing reply legacy logged as "No valid response
// from ECU" (e.g. check_romcrc():853, init_flash_write():930) and stopped.
Result<bytes::Bytes> kernel_exchange(IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events,
                                     bytes::ByteView request, std::chrono::milliseconds timeout, std::string_view what)
{
    Result<bytes::Bytes> reply = required_exchange(transport, clock, cancellation, request, 0ms, timeout, what);
    if (!reply.has_value() && reply.error().kind == ErrorKind::Timeout)
    {
        events.log(LogLevel::Error, "No valid response from ECU");
    }
    return reply;
}

// The legacy write-path reply check: `received.length() > min_size - 1`, else
// "No valid response from ECU"; then at(0), at(1), at(4) against the positive
// reply, else "Wrong response from ECU" -- with the NRC from mid(offset) where
// legacy appended parse_nrc_message() (flash_block():1180-1182).
Status require_kernel_reply(const bytes::Bytes& reply, std::uint8_t opcode, std::size_t min_size, IEventSink& events,
                            std::string_view what, std::optional<std::size_t> nrc_offset = std::nullopt)
{
    if (reply.size() < min_size)
    {
        events.log(LogLevel::Error, "No valid response from ECU");
        return fail(ErrorKind::BadResponse, std::format("No valid response from ECU during {}", what));
    }
    if (!kernel_reply_ok(reply, opcode, min_size))
    {
        if (nrc_offset.has_value())
        {
            log_wrong_response(events, reply, *nrc_offset);
        }
        else
        {
            events.log(LogLevel::Error, "Wrong response from ECU");
        }
        return fail(ErrorKind::BadResponse, std::format("Wrong response from ECU during {}", what));
    }
    return {};
}

// check_romcrc():812-831 -- CRC [addr32, 00, len24], serial_read_extra_long_timeout.
// :874/:880 -- a 200ms flush read after either compare outcome (not after a
// rejected reply, which returns first).
Result<std::uint32_t> read_block_crc(IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events,
                                     const MemoryRegion& block)
{
    const bytes::Bytes request = frame(kOpCrc, composeBe(block.start, 0x00_b, u24(block.length)));
    Result<bytes::Bytes> reply =
        kernel_exchange(transport, clock, cancellation, events, request, kExtraLongTimeout, "CRC check");
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    // :833-856. Correction: legacy checked `> 5` then read at(5..8); require
    // all nine bytes before parsing.
    if (Status ok = require_kernel_reply(*reply, kOpCrc, 9, events, "CRC check"); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    const std::uint32_t crc = bytes::readU32Be(*reply, 5);
    if (Status cancelled = check_cancelled(cancellation, "cancelled before CRC flush"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (Result<OptionalBytes> flushed = transport.read(kShortTimeout, cancellation); !flushed.has_value())
    {
        return std::unexpected(flushed.error());
    }
    return crc;
}

// get_changed_blocks():767-791 and check_romcrc():858-879.
Result<std::vector<bool>> compare_blocks(IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events,
                                         const flashdev_t& device, bytes::ByteView image)
{
    std::vector<bool> changed(device.numblocks, false);
    for (unsigned i = 0; i < device.numblocks; ++i)
    {
        // :769-772 stopRequested().
        if (Status cancelled = check_cancelled(cancellation, "cancelled during ROM compare"); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        const MemoryRegion block{device.fblocks[i].start, device.fblocks[i].len};
        events.log(LogLevel::Info, std::format("FB{:02}\t0x{:08X}\t0x{:08X}", i, block.start, block.length)); // :782
        Result<std::uint32_t> ecu_crc = read_block_crc(transport, clock, cancellation, events, block);
        if (!ecu_crc.has_value())
        {
            return std::unexpected(ecu_crc.error());
        }
        const std::uint32_t image_crc = fastecu::checksum::crc32(image.subspan(block.start, block.length));
        changed[i] = *ecu_crc != image_crc;
        events.log(LogLevel::Debug, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", *ecu_crc, image_crc)); // :860
        events.log(LogLevel::Info, std::format("\t{:08X}\t{:08X}", *ecu_crc, image_crc));                     // :868
        events.log(LogLevel::Info, changed[i] ? "\tNO" : "\tYES");                              // :872/:878
        if (Status slept = sleep_for(clock, cancellation, kCrcBlockPacing); !slept.has_value()) // :789
        {
            return std::unexpected(slept.error());
        }
    }
    return changed;
}

// write_mem():671-680 / :725-734.
unsigned log_changed_blocks(IEventSink& events, const std::vector<bool>& changed)
{
    unsigned count = 0;
    events.log(LogLevel::Info, "Different blocks : ");
    for (std::size_t i = 0; i < changed.size(); ++i)
    {
        if (changed[i])
        {
            events.log(LogLevel::Info, std::format("{}, ", i));
            ++count;
        }
    }
    events.log(LogLevel::Info, std::format(" (total: {})", count));
    return count;
}

// init_flash_write():893-1026. The sizes are queried and logged only; legacy
// flash_block() hard-codes 0x200/0x1000. The mode ack gates everything after.
Status init_flash_write(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                        IEventSink& events, bool test_write)
{
    for (const std::uint8_t opcode : {kOpGetMaxMsgSize, kOpGetMaxBlockSize})
    {
        // :893 / :935.
        events.log(LogLevel::Info, opcode == kOpGetMaxMsgSize ? "Check max message length" : "Check flashblock size");
        Result<bytes::Bytes> reply =
            kernel_exchange(transport, clock, cancellation, events, frame(opcode), kMediumTimeout, "flash init");
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        // :907-933 / :949-975 -- `> 9`, value at(5..8).
        if (Status ok = require_kernel_reply(*reply, opcode, 10, events, "flash init"); !ok.has_value())
        {
            return ok;
        }
        events.log(LogLevel::Info, std::format(": 0x{:04x}", bytes::readU32Be(*reply, 5))); // :916 / :958
    }
    // :977-987.
    const std::uint8_t mode = test_write ? kOpFlashDisable : kOpFlashEnable;
    events.log(LogLevel::Info, test_write ? "Test write mode on, no actual flash write is performed"
                                          : "Test write mode off, perform actual flash write");
    Result<bytes::Bytes> reply =
        kernel_exchange(transport, clock, cancellation, events, frame(mode), kMediumTimeout, "flash mode");
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    // :1002-1022 -- `> 5`; a rejected mode stops the write (legacy returned
    // STATUS_ERROR, and write_mem() stopped before any erase).
    if (Status ok = require_kernel_reply(*reply, mode, 6, events, "flash mode"); !ok.has_value())
    {
        return ok;
    }
    events.log(LogLevel::Error, "Flash mode succesfully set"); // :1008, legacy LOG_E
    return {};
}

std::uint64_t elapsed_milliseconds(std::chrono::steady_clock::time_point start,
                                   std::chrono::steady_clock::time_point end)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 1U;
}

// flash_block():1125-1361. BLANK_PAGE is sent in both modes, as in legacy;
// then 0x200-byte WRITE_FLASH_BUFFER chunks, and at each 0x1000 boundary a
// COMMIT (write) or VALIDATE (test write) carrying the image CRC32.
Status flash_block(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                   IEventSink& events, bytes::ByteView image, const MemoryRegion& block, bool test_write,
                   std::uint64_t& written, std::uint64_t total)
{
    // :1148-1195.
    events.log(LogLevel::Info, std::format("Flash page erase addr: 0x{:08x} len: 0x{:08x}", block.start, block.length));
    events.log(LogLevel::Info, "Erasing flash page...");
    Result<bytes::Bytes> erased =
        kernel_exchange(transport, clock, cancellation, events, frame(kOpBlankPage, composeBe(block.start)),
                        kExtraLongTimeout, "erase");
    if (!erased.has_value())
    {
        return std::unexpected(erased.error());
    }
    if (Status ok = require_kernel_reply(*erased, kOpBlankPage, 5, events, "erase", 8); !ok.has_value())
    {
        return ok;
    }
    events.log(LogLevel::Info, " erased");
    events.log(LogLevel::Info,
               std::format("Start flash write addr: 0x{:08x} len: 0x{:08x}", block.start, block.length)); // :1197

    std::uint32_t commit_start = block.start;
    for (std::uint32_t offset = 0; offset < block.length; offset += kWriteChunkSize)
    {
        const auto chunk_started = clock.now();
        // :1206-1209 stopRequested().
        if (Status cancelled = check_cancelled(cancellation, "cancelled during flash write"); !cancelled.has_value())
        {
            return cancelled;
        }
        // :1211-1252 -- `> 5`.
        const std::uint32_t address = block.start + offset;
        Result<bytes::Bytes> chunk =
            kernel_exchange(transport, clock, cancellation, events,
                            frame(kOpWriteFlashBuffer, composeBe(address, image.subspan(address, kWriteChunkSize))),
                            kExtraLongTimeout, "write");
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        if (Status ok = require_kernel_reply(*chunk, kOpWriteFlashBuffer, 6, events, "write"); !ok.has_value())
        {
            return ok;
        }
        events.log(LogLevel::Debug, "Data written to flash buffer"); // :1238
        // :1254-1286. Legacy printed the previous chunk's speed (and an
        // uninitialised one first); this chunk's is printed instead.
        const std::uint64_t elapsed_ms = elapsed_milliseconds(chunk_started, clock.now());
        auto curspeed = static_cast<unsigned>(kWriteChunkSize * (1000.0F / static_cast<float>(elapsed_ms)));
        if (curspeed == 0)
        {
            curspeed = 1;
        }
        written += kWriteChunkSize;
        auto tleft = static_cast<unsigned>(static_cast<float>(total - written) / static_cast<float>(curspeed));
        if (tleft > 9999U)
        {
            tleft = 9999U;
        }
        ++tleft;
        events.log(LogLevel::Info, std::format("Write flash buffer: 0x{:08X} ({}% - {} B/s, ~ {} s remain)", address,
                                               (100U * offset) / block.length, curspeed, tleft));
        events.progress(static_cast<int>(written), static_cast<int>(total)); // :1288-1289

        // :1291-1358.
        if (commit_start + kCommitBlockSize == address + kWriteChunkSize)
        {
            const std::uint32_t crc = fastecu::checksum::crc32(image.subspan(commit_start, kCommitBlockSize));
            events.log(LogLevel::Info, "Flash buffer write complete... ");
            events.log(LogLevel::Debug, std::format("Image CRC32: 0x{:x}", crc));
            const std::uint8_t commit = test_write ? kOpValidateFlashBuffer : kOpCommitFlashBuffer;
            events.log(LogLevel::Info, test_write ? std::format("Validate flash addr: 0x{:x}", commit_start)
                                                  : std::format("Committ flash addr: 0x{:x}", commit_start));
            events.log(LogLevel::Info, std::format(" len: 0x{:x}", kCommitBlockSize));
            events.log(LogLevel::Info, std::format(" crc32: 0x{:x}", crc));
            Result<bytes::Bytes> committed = kernel_exchange(
                transport, clock, cancellation, events,
                frame(commit, composeBe(commit_start, static_cast<std::uint16_t>(kCommitBlockSize), crc)),
                kExtraLongTimeout, "commit");
            if (!committed.has_value())
            {
                return std::unexpected(committed.error());
            }
            if (Status ok = require_kernel_reply(*committed, commit, 6, events, "commit"); !ok.has_value())
            {
                return ok;
            }
            commit_start += kCommitBlockSize;
        }
    }
    return {};
}

// reflash_block():1034-1118 -- init on the first block, then PROG_VOLT and
// flash_block().
Status reflash_block(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                     IEventSink& events, bytes::ByteView image, const MemoryRegion& block, bool test_write,
                     bool& flash_write_init, std::uint64_t& written, std::uint64_t total)
{
    // :1046-1052.
    if (!flash_write_init)
    {
        if (Status ok = init_flash_write(transport, clock, cancellation, events, test_write); !ok.has_value())
        {
            return ok;
        }
        flash_write_init = true;
    }
    events.log(LogLevel::Info,
               std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length)); // :1065
    // :1068-1105 -- `> 9`, voltage at(5..6) / 50.
    events.log(LogLevel::Info, "Check flash voltage");
    Result<bytes::Bytes> volt =
        kernel_exchange(transport, clock, cancellation, events, frame(kOpProgVolt), kMediumTimeout, "prog voltage");
    if (!volt.has_value())
    {
        return std::unexpected(volt.error());
    }
    if (Status ok = require_kernel_reply(*volt, kOpProgVolt, 10, events, "prog voltage"); !ok.has_value())
    {
        return ok;
    }
    // QString::number(float) is 'g' with precision 6, as is std::format's {:g}.
    const auto prog_voltage = static_cast<float>(bytes::readU16Be(*volt, 5) / 50.0);
    events.log(LogLevel::Info, std::format(": {:g}V", prog_voltage));

    // :1107-1115.
    if (Status flashed = flash_block(transport, clock, cancellation, events, image, block, test_write, written, total);
        !flashed.has_value())
    {
        events.log(LogLevel::Error, "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is "
                                    "most likely still running and receiving commands!");
        return flashed;
    }
    events.log(LogLevel::Info, "Flash block ok");
    return {};
}

// write_mem():641-755.
Status write_mem(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                 IEventSink& events, const FlashPlan& plan)
{
    const flashdev_t *device = find_flash_device(plan.mcu_name());
    if (device == nullptr || !plan.image().has_value() || plan.image()->size() < device->romsize)
    {
        return fail(ErrorKind::InvalidConfig, "Denso SH705x K-Line write needs the full ROM image");
    }
    // Legacy indexed the image by absolute flash address (fblocks[0].start is
    // 0 for every SH705x device), for the compare and the write alike.
    const bytes::ByteView image = *plan.image();
    const bool test_write = plan.operation() == FlashOperation::TestWrite;

    events.log(LogLevel::Info, "--- Comparing ECU flash memory pages to image file ---"); // :661
    events.log(LogLevel::Info, "blk\tstart\tlen\tecu crc\timg crc\tsame?");               // :662
    Result<std::vector<bool>> changed = compare_blocks(transport, clock, cancellation, events, *device, image);
    if (!changed.has_value())
    {
        events.log(LogLevel::Error, "Error in ROM compare"); // :666
        return std::unexpected(changed.error());
    }
    if (log_changed_blocks(events, *changed) == 0)
    {
        events.log(LogLevel::Info,
                   "*** Compare results no difference between ROM and ECU data, no flashing needed! ***"); // :751
        return {};
    }

    // :684-693.
    std::uint64_t total = 0;
    for (unsigned i = 0; i < device->numblocks; ++i)
    {
        if ((*changed)[i])
        {
            total += device->fblocks[i].len;
        }
    }
    events.log(LogLevel::Info, "--- Start writing ROM file to ECU flash memory ---"); // :695
    events.progress(0, static_cast<int>(total));                                      // :654
    bool flash_write_init = false;
    std::uint64_t written = 0;
    for (unsigned i = 0; i < device->numblocks; ++i)
    {
        if (!(*changed)[i])
        {
            continue;
        }
        const MemoryRegion block{device->fblocks[i].start, device->fblocks[i].len};
        if (Status ok = reflash_block(transport, clock, cancellation, events, image, block, test_write,
                                      flash_write_init, written, total);
            !ok.has_value())
        {
            events.log(LogLevel::Info, std::format("Block {} reflash failed.", i)); // :703
            return ok;
        }
        events.log(LogLevel::Info, std::format("Block {} reflash complete.", i)); // :709
    }

    events.log(LogLevel::Info, "--- Comparing ECU flash memory pages to image file after reflash ---"); // :715
    events.log(LogLevel::Info, "blk\tstart\tlen\tecu crc\timg crc\tsame?");                             // :716
    Result<std::vector<bool>> after = compare_blocks(transport, clock, cancellation, events, *device, image);
    if (!after.has_value())
    {
        events.log(LogLevel::Error, "Error in ROM compare"); // :720
        return std::unexpected(after.error());
    }
    const unsigned still_different = log_changed_blocks(events, *after);
    if (test_write)
    {
        // :746 -- a validate-only pass leaves the flash unchanged, so the
        // post-compare still differs by design.
        events.log(LogLevel::Info, "*** Test write PASS, it's ok to perform actual write! ***");
        return {};
    }
    if (still_different != 0)
    {
        events.log(LogLevel::Error, "*** ERROR IN FLASH PROCESS ***"); // :739
        events.log(LogLevel::Error,
                   "Don't power off your ECU, kernel is still running and you can try flashing again!"); // :740
        // Correction: legacy returned STATUS_SUCCESS here (:754).
        return fail(ErrorKind::BadResponse, "ECU flash still differs from the image after reflash");
    }
    return {};
}

} // namespace

bytes::Bytes denso_sh705x_kline_balanced_kernel(bytes::ByteView kernel)
{
    // upload_kernel():361-387.
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

Result<KlineConfig> SubaruDensoSh705xKlineExecutor::transport_setup(const FlashPlan& plan) const
{
    if (Status match = check_family(plan, FlashFamily::SubaruDensoSh705xKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_denso_sh705x_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return non_iso14230_kline_config_from(std::get<SubaruDensoSh705xKlinePlan>(plan.family_plan()));
}

Status SubaruDensoSh705xKlineExecutor::before_transport_configure(IKlineFlashTransport& transport, IClock&,
                                                                  const ICancellationToken& cancellation) const
{
    // execute():67 -- serial->reset_connection() before every setter.
    if (Status cancelled = check_cancelled(cancellation, "cancelled before K-Line reset"); !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status reset = transport.reset_connection(); !reset.has_value())
    {
        return reset;
    }
    return check_cancelled(cancellation, "cancelled after K-Line reset");
}

Result<FlashExecutionResult> SubaruDensoSh705xKlineExecutor::execute(const FlashPlan& plan,
                                                                     IKlineFlashTransport& transport, IClock& clock,
                                                                     const ICancellationToken& cancellation,
                                                                     IEventSink& events)
{
    if (Status match = check_family(plan, FlashFamily::SubaruDensoSh705xKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_denso_sh705x_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    // execute():68 -- the shared serial can arrive with the header flag set by
    // another feature; every BEEF and SSM frame of this family goes out bare.
    if (Status header = transport.set_add_iso14230_header(false); !header.has_value())
    {
        return std::unexpected(header.error());
    }
    Result<std::optional<std::string>> ecu_id = start_session(transport, clock, cancellation, events, plan);
    if (!ecu_id.has_value())
    {
        return std::unexpected(ecu_id.error());
    }
    if (plan.operation() == FlashOperation::Read)
    {
        // :92-93 -- externalLoggerMessage() precedes the LOG_I() the brief
        // cites; every ported sibling executor keeps both (e.g.
        // mitsu_colt_m32r_can_executor.cpp:709-710). Not in the task-7 brief's
        // step-3 snippet; added here to match legacy and the established
        // pattern (see task-7-report.md).
        events.notice("Reading ROM, please wait...");
        events.log(LogLevel::Info, "Reading ROM from Subaru 04 32-bit using K-Line");
        Result<bytes::Bytes> rom = read_mem(transport, clock, cancellation, events, plan.transfer_region());
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        std::optional<std::string> rom_id;
        if (ecu_id->has_value())
        {
            rom_id = **ecu_id + "_"; // connect_bootloader(): RomId = ecuid + "_"
        }
        return FlashExecutionResult{.operation = plan.operation(), .read_bytes = std::move(*rom), .rom_id = rom_id};
    }
    // :96-101 -- test_write and write share write_mem().
    events.notice("Writing ROM, please wait...");
    events.log(LogLevel::Info, "Writing ROM to Subaru 04 32-bit using K-Line");
    if (Status written = write_mem(transport, clock, cancellation, events, plan); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{.operation = plan.operation(), .read_bytes = std::nullopt};
}

} // namespace fastecu::flash
