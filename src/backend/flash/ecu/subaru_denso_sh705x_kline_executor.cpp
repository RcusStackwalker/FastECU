#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.h"

#include <chrono>
#include <format>
#include <string>
#include <utility>

#include "src/algorithms/checksum/checksum_primitives.h"
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

Result<FlashExecutionResult> SubaruDensoSh705xKlineExecutor::execute(const FlashPlan& plan, IKlineFlashTransport&,
                                                                     IClock&, const ICancellationToken&, IEventSink&)
{
    if (Status match = check_family(plan, FlashFamily::SubaruDensoSh705xKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_denso_sh705x_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return fail(ErrorKind::Unsupported, "Denso SH705x K-Line execute() is completed in wave 6b-2 tasks 6-8");
}

} // namespace fastecu::flash
