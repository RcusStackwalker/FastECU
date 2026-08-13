#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.h"

#include <algorithm>
#include <utility>

#include "src/algorithms/checksum/checksum_primitives.h"

namespace fastecu::flash
{
namespace
{

constexpr std::uint16_t kStartComm = 0xBEEF;
constexpr std::uint8_t kOpId = 0x01;
constexpr std::uint8_t kOpUploadKernel = 0x53;

Status check_cancelled(const ICancellationToken& cancellation, std::string detail)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::move(detail));
    }
    return {};
}

// Shared SUB_KERNEL_START_COMM shape, from legacy
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1150-1159:
// [0xBE][0xEF][len+1 hi][len+1 lo][opcode][payload][checksum8].
bytes::Bytes frame(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes out{static_cast<bytes::Byte>((kStartComm >> 8) & 0xFF),
                     static_cast<bytes::Byte>(kStartComm & 0xFF)};
    const std::uint16_t len_plus_one = static_cast<std::uint16_t>(payload.size() + 1);
    out.push_back(static_cast<bytes::Byte>((len_plus_one >> 8) & 0xFF));
    out.push_back(static_cast<bytes::Byte>(len_plus_one & 0xFF));
    out.push_back(opcode);
    out.insert(out.end(), payload.begin(), payload.end());
    out.push_back(fastecu::checksum::checksum8(out, false));
    return out;
}

bool response_ok(bytes::ByteView received, std::uint8_t expected_opcode_with_ack)
{
    return received.size() > 4 &&
           received[0] == static_cast<bytes::Byte>((kStartComm >> 8) & 0xFF) &&
           received[1] == static_cast<bytes::Byte>(kStartComm & 0xFF) &&
           received[4] == expected_opcode_with_ack;
}

// Shared write/read exchange, porting legacy
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:116-119 and 269-270.
Result<bytes::Bytes> exchange(IKlineFlashTransport& transport, IClock& clock,
                              const ICancellationToken& cancellation, bytes::ByteView request,
                              int settle_ms, int timeout_ms)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before write");
        !cancelled.has_value())
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
    if (Status cancelled = check_cancelled(cancellation, "cancelled after write");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (settle_ms > 0)
    {
        if (Status slept = clock.sleep(settle_ms, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before read");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    auto received = transport.read(timeout_ms, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after read");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from ECU");
    }
    return std::move(**received);
}

// Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168:
// frame a kernel-ID probe, settle 200ms, then read with the long timeout.
// A no-frame response means the kernel is not alive yet, not an I/O failure.
Result<bytes::Bytes> request_kernel_id(IKlineFlashTransport& transport, IClock& clock,
                                       const ICancellationToken& cancellation)
{
    const bytes::Bytes request = frame(kOpId);
    if (Status cancelled = check_cancelled(cancellation, "cancelled before write");
        !cancelled.has_value())
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
    if (Status cancelled = check_cancelled(cancellation, "cancelled after write");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (Status slept = clock.sleep(200, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before read");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    auto received = transport.read(2000, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after read");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (!received->has_value())
    {
        return bytes::Bytes{};
    }
    return std::move(**received);
}

bool looks_kernel_alive(bytes::ByteView received)
{
    return response_ok(received, static_cast<bytes::Byte>(kOpId | 0x40));
}

} // namespace

Status SubaruDensoMc68hc16y5_02Executor::connect_bootloader(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events, const SubaruDensoMc68hc16y5_02Plan& family_plan, bool& kernel_alive)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before connect");
        !cancelled.has_value())
    {
        return cancelled;
    }
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-119.
    if (Status slept = clock.sleep(200, cancellation); !slept.has_value())
    {
        return slept;
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before LEC pulse");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status pulsed = transport.pulse_lec_2_line(200); !pulsed.has_value())
    {
        return pulsed;
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after LEC pulse");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status slept = clock.sleep(200, cancellation); !slept.has_value())
    {
        return slept;
    }
    events.log(LogLevel::Info, "Connecting to Subaru 01-05 16-bit K-Line bootloader...");
    const bytes::Bytes init_request{0x4D, 0xFF, 0xB4};
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:115-146.
    Result<bytes::Bytes> init_response =
        exchange(transport, clock, cancellation, init_request, 50, 200);
    if (!init_response.has_value())
    {
        return std::unexpected(init_response.error());
    }
    if (init_response->size() >= family_plan.bootloader_ok.size() &&
        std::equal(family_plan.bootloader_ok.begin(), family_plan.bootloader_ok.end(),
                   init_response->begin()))
    {
        events.log(LogLevel::Info, "Connected to bootloader");
        kernel_alive = false;
        return {};
    }

    events.log(LogLevel::Warning, "Bad response from bootloader, checking for a running kernel...");
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:149-151.
    if (Status slept = clock.sleep(100, cancellation); !slept.has_value())
    {
        return slept;
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before disabling LEC lines");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status disabled = transport.disable_lec_lines(); !disabled.has_value())
    {
        return disabled;
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after disabling LEC lines");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before changing baud");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status baud = transport.setBaud(62500); !baud.has_value())
    {
        return baud;
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after changing baud");
        !cancelled.has_value())
    {
        return cancelled;
    }
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:149-179 and 1139-1168.
    Result<bytes::Bytes> probe = request_kernel_id(transport, clock, cancellation);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (probe->size() <= 4)
    {
        return fail(ErrorKind::BadResponse, "No valid response from ECU");
    }
    if (!looks_kernel_alive(*probe))
    {
        return fail(ErrorKind::BadResponse, "Wrong response from ECU");
    }
    kernel_alive = true;
    events.log(LogLevel::Info, "Kernel already running");
    return {};
}

Status SubaruDensoMc68hc16y5_02Executor::upload_kernel(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events, const SubaruDensoMc68hc16y5_02Plan& family_plan, const KernelImage& kernel)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before kernel upload");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (kernel.bytes.empty())
    {
        return fail(ErrorKind::InvalidConfig, "kernel image is empty");
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before changing baud");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status baud = transport.setBaud(family_plan.kernel_baud); !baud.has_value())
    {
        return baud;
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after changing baud");
        !cancelled.has_value())
    {
        return cancelled;
    }

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:220-250:
    // pad to 16 bytes, encrypt every byte, then replace encrypted[2..3].
    bytes::Bytes payload = kernel.bytes;
    while (payload.size() % 0x10 != 0)
    {
        payload.push_back(0x00);
    }
    for (auto& byte : payload)
    {
        byte = static_cast<bytes::Byte>((byte ^ family_plan.encryption_xor) + 0x10);
    }
    payload[2] = static_cast<bytes::Byte>((family_plan.kernel_magic >> 8) & 0xFF);
    payload[3] = static_cast<bytes::Byte>(family_plan.kernel_magic & 0xFF);

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:256-281:
    // SUB_UPLOAD_KERNEL is raw, not SUB_KERNEL_START_COMM-framed.
    const std::uint32_t address = kernel.load_address;
    const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
    bytes::Bytes request{kOpUploadKernel,
                         static_cast<bytes::Byte>((address >> 16) & 0xFF),
                         static_cast<bytes::Byte>((address >> 8) & 0xFF),
                         static_cast<bytes::Byte>((length >> 16) & 0xFF),
                         static_cast<bytes::Byte>((length >> 8) & 0xFF),
                         static_cast<bytes::Byte>(length & 0xFF)};
    request.insert(request.end(), payload.begin(), payload.end());
    request.push_back(fastecu::checksum::checksum8(request, true));

    events.log(LogLevel::Info, "Sending kernel...");
    Result<bytes::Bytes> upload_response = exchange(transport, clock, cancellation, request, 0, 200);
    if (!upload_response.has_value())
    {
        return std::unexpected(upload_response.error());
    }
    if (!upload_response->empty())
    {
        return fail(ErrorKind::BadResponse, "Error on kernel upload");
    }
    events.log(LogLevel::Info, "Kernel uploaded successfully");

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:287-317 and 1139-1168.
    if (Status slept = clock.sleep(1500, cancellation); !slept.has_value())
    {
        return slept;
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before changing baud");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status baud = transport.setBaud(62500); !baud.has_value())
    {
        return baud;
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after changing baud");
        !cancelled.has_value())
    {
        return cancelled;
    }
    Result<bytes::Bytes> id = request_kernel_id(transport, clock, cancellation);
    if (!id.has_value())
    {
        return std::unexpected(id.error());
    }
    if (id->size() <= 4)
    {
        return fail(ErrorKind::BadResponse, "No valid response from ECU");
    }
    if (!looks_kernel_alive(*id))
    {
        return fail(ErrorKind::BadResponse, "Wrong response from ECU");
    }
    events.log(LogLevel::Info, "Kernel is alive");
    return {};
}

Result<FlashExecutionResult> SubaruDensoMc68hc16y5_02Executor::execute(
    const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events)
{
    if (Status match = check_family_transport_match(plan, FlashFamily::SubaruDensoMc68hc16y5_02,
                                                    TransportKind::Kline);
        !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before transport configuration");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    auto *kline_ptr = dynamic_cast<IKlineFlashTransport *>(&transport);
    if (kline_ptr == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "transport does not implement IKlineFlashTransport");
    }
    const auto& family_plan = std::get<SubaruDensoMc68hc16y5_02Plan>(plan.family_plan());
    IKlineFlashTransport& kline = *kline_ptr;

    if (Status configured = kline.configure(KlineConfig{.baud = family_plan.connect_baud,
                                                        .iso14230 = false,
                                                        .tester_id = 0,
                                                        .target_id = 0});
        !configured.has_value())
    {
        return std::unexpected(configured.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after transport configuration");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (Status opened = kline.open(); !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    Result<FlashExecutionResult> phase_result = [&]() -> Result<FlashExecutionResult>
    {
        if (Status cancelled = check_cancelled(cancellation, "cancelled after opening transport");
            !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:67-70.
        if (Status cancelled = check_cancelled(cancellation, "cancelled before disabling LEC lines");
            !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        if (Status disabled = kline.disable_lec_lines(); !disabled.has_value())
        {
            return std::unexpected(disabled.error());
        }
        if (Status cancelled = check_cancelled(cancellation, "cancelled after disabling LEC lines");
            !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }

        bool kernel_alive = false;
        if (Status connected = connect_bootloader(kline, clock, cancellation, events, family_plan,
                                                  kernel_alive);
            !connected.has_value())
        {
            return std::unexpected(connected.error());
        }
        if (!kernel_alive)
        {
            if (!plan.kernel().has_value())
            {
                return fail(ErrorKind::InvalidConfig, "MC68HC16Y5_02 requires a kernel image");
            }
            if (Status uploaded = upload_kernel(kline, clock, cancellation, events, family_plan,
                                                *plan.kernel());
                !uploaded.has_value())
            {
                return std::unexpected(uploaded.error());
            }
        }
        return fail(ErrorKind::Unsupported, "read/write phase not yet implemented");
    }();

    Status close_status = kline.close();
    if (!phase_result.has_value() && phase_result.error().kind != ErrorKind::Unsupported)
    {
        if (!close_status.has_value())
        {
            events.log(LogLevel::Warning, "close failed after MC68HC16Y5_02 phase error");
        }
        return std::unexpected(phase_result.error());
    }
    if (!close_status.has_value())
    {
        return std::unexpected(close_status.error());
    }
    return std::unexpected(phase_result.error());
}

} // namespace fastecu::flash
