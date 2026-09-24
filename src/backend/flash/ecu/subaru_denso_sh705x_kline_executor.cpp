#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

} // namespace

bytes::Bytes denso_sh705x_kline_balanced_kernel(bytes::ByteView kernel)
{
    // upload_kernel():361-387.
    bytes::Bytes out(kernel.begin(), kernel.end());
    out.push_back(0x00);
    out.push_back(0x00);
    out.resize((out.size() + 3) & ~std::size_t{3}, 0x00);
    out.resize(out.size() - 2);
    const auto at = [&out](std::size_t index) -> std::uint32_t { return index < out.size() ? out[index] : 0U; };
    std::uint16_t sum = 0;
    for (std::size_t i = 0; i < out.size(); i += 4)
    {
        sum = static_cast<std::uint16_t>(sum + ((at(i) << 24) | (at(i + 1) << 16) | (at(i + 2) << 8) | at(i + 3)));
    }
    const auto balance = static_cast<std::uint16_t>(0x5AA5 - sum);
    out.push_back(static_cast<bytes::Byte>(balance >> 8));
    out.push_back(static_cast<bytes::Byte>(balance & 0xFF));
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
    Result<std::optional<std::string>> ecu_id = start_session(transport, clock, cancellation, events, plan);
    if (!ecu_id.has_value())
    {
        return std::unexpected(ecu_id.error());
    }
    return fail(ErrorKind::Unsupported, "Denso SH705x K-Line operation tail lands in tasks 7-8");
}

} // namespace fastecu::flash
