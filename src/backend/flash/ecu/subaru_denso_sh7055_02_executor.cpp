#include "src/backend/flash/ecu/subaru_denso_sh7055_02_executor.h"

#include <format>
#include <utility>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

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

// Shared SUB_KERNEL_START_COMM wire shape. Legacy
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:1180-1188:
// [0xBE][0xEF][length high][length low][opcode][payload][checksum8].
bytes::Bytes frame(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes out{static_cast<bytes::Byte>((kStartComm >> 8) & 0xFF),
                     static_cast<bytes::Byte>(kStartComm & 0xFF)};
    const std::uint16_t length = static_cast<std::uint16_t>(payload.size() + 1);
    bytes::appendU16Be(out, length);
    out.push_back(opcode);
    out.insert(out.end(), payload.begin(), payload.end());
    out.push_back(fastecu::checksum::checksum8(out, false));
    return out;
}

bool response_ok(bytes::ByteView received, std::uint8_t expected_opcode)
{
    return received.size() > 4 && received[0] == 0xBE && received[1] == 0xEF &&
           received[4] == expected_opcode;
}

// Shared cancellation-aware write/read exchange. Legacy
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:210-212,
// 297-306, 1189-1192, and 1211-1216.
Result<IKlineFlashTransport::OptionalBytes> exchange(
    IKlineFlashTransport& transport, IClock *clock, const ICancellationToken& cancellation,
    bytes::ByteView request, int settle_ms, int timeout_ms)
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
    if (clock != nullptr && settle_ms > 0)
    {
        if (Status slept = clock->sleep(settle_ms, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before read");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    Result<IKlineFlashTransport::OptionalBytes> received =
        transport.read(timeout_ms, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after read");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    return received;
}

// Read-only drain used where legacy intentionally discarded stale/echo bytes.
// Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:69-70,
// 170-172, 194-195, 218-220, and 225-228.
Status drain(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
             int timeout_ms, std::string detail)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before " + detail);
        !cancelled.has_value())
    {
        return cancelled;
    }
    Result<IKlineFlashTransport::OptionalBytes> drained = transport.read(timeout_ms, cancellation);
    if (!drained.has_value())
    {
        return std::unexpected(drained.error());
    }
    return check_cancelled(cancellation, "cancelled after " + detail);
}

// Kernel-ID exchange. Legacy
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:1169-1198.
Result<bytes::Bytes> request_kernel_id(IKlineFlashTransport& transport, IClock& clock,
                                       const ICancellationToken& cancellation)
{
    const bytes::Bytes request = frame(kOpId);
    Result<IKlineFlashTransport::OptionalBytes> received =
        exchange(transport, &clock, cancellation, request, 200, 2000);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    return received->has_value() ? std::move(**received) : bytes::Bytes{};
}

std::string ecu_id_hex(bytes::ByteView id)
{
    std::string result;
    result.reserve(id.size() * 2);
    for (const bytes::Byte byte : id)
    {
        result += std::format("{:02X}", byte);
    }
    return result;
}

Status change_baud(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                   int baud)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before changing baud");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status changed = transport.setBaud(baud); !changed.has_value())
    {
        return changed;
    }
    return check_cancelled(cancellation, "cancelled after changing baud");
}

} // namespace

Status SubaruDensoSh7055_02Executor::connect_bootloader(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events, const SubaruDensoSh7055_02Plan& family_plan, bool read_ecu_id,
    bool& kernel_alive, std::optional<std::string>& ecu_id)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before connect");
        !cancelled.has_value())
    {
        return cancelled;
    }
    events.log(LogLevel::Info, "Checking if kernel is already running...");
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:108-131 and 1169-1198.
    Result<bytes::Bytes> probe = request_kernel_id(transport, clock, cancellation);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (response_ok(*probe, static_cast<bytes::Byte>(kOpId | 0x40)))
    {
        kernel_alive = true;
        events.log(LogLevel::Info, "Kernel already running");
        return {};
    }

    if (read_ecu_id)
    {
        if (Status baud = change_baud(transport, cancellation, 4800); !baud.has_value())
        {
            return baud;
        }
        events.log(LogLevel::Info, "Requesting ECU ID");
        const bytes::Bytes request = SsmProtocol::addHeader(
            bytes::Bytes{0xBF}, family_plan.tester_id, family_plan.target_id, false);
        // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:133-168 and 1206-1218.
        Result<IKlineFlashTransport::OptionalBytes> received =
            exchange(transport, nullptr, cancellation, request, 0, 2000);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            return fail(ErrorKind::Timeout, "no response from ECU during ID read");
        }
        const bytes::Bytes& response = **received;
        if (response.size() <= 4 || response[4] != 0xFF)
        {
            return fail(ErrorKind::BadResponse, "wrong response from ECU during ID read");
        }
        if (response.size() < 13)
        {
            return fail(ErrorKind::BadResponse, "ECU ID response is too short");
        }
        ecu_id = ecu_id_hex(bytes::ByteView(response).subspan(8, 5));
        events.log(LogLevel::Info, "ECU ID: " + *ecu_id);
    }

    if (Status baud = change_baud(transport, cancellation, 9600); !baud.has_value())
    {
        return baud;
    }
    // The plan's upfront CycleIgnition confirmation replaces the legacy
    // executor-side human prompt at
    // src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:170-173.
    // No prompt occurs here.
    if (Status cancelled = check_cancelled(cancellation, "cancelled before disabling LEC lines");
        !cancelled.has_value())
    {
        return cancelled;
    }
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:170-172.
    if (Status disabled = transport.disable_lec_lines(); !disabled.has_value())
    {
        return disabled;
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after disabling LEC lines");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status drained = drain(transport, cancellation, 10, "pre-countdown drain");
        !drained.has_value())
    {
        return drained;
    }

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:188-196 and 1300-1314.
    for (int seconds_left = 3; seconds_left > 0; --seconds_left)
    {
        events.log(LogLevel::Info, "Starting in " + std::to_string(seconds_left));
        if (Status slept = clock.sleep(1000, cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    events.log(LogLevel::Info, "Switch Ignition ON!");
    if (Status slept = clock.sleep(250, cancellation); !slept.has_value())
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
    if (Status drained = drain(transport, cancellation, 10, "post-pulse drain");
        !drained.has_value())
    {
        return drained;
    }
    if (Status slept = clock.sleep(190, cancellation); !slept.has_value())
    {
        return slept;
    }

    const bytes::Bytes init_request{0x4D, 0xFF, 0xB4};
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:198-228.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (Status cancelled = check_cancelled(cancellation, "cancelled during WRX init");
            !cancelled.has_value())
        {
            return cancelled;
        }
        Result<IKlineFlashTransport::OptionalBytes> received =
            exchange(transport, nullptr, cancellation, init_request, 0, 10);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        const bool matches_expected = received->has_value() && (**received).size() >= 3 &&
                                      (**received)[0] == 0x4D && (**received)[1] == 0x00 &&
                                      (**received)[2] == 0xB3;
        if (!matches_expected)
        {
            events.log(LogLevel::Info, "Connected to bootloader");
            if (Status slept = clock.sleep(100, cancellation); !slept.has_value())
            {
                return slept;
            }
            if (Status drained = drain(transport, cancellation, 10, "post-connect drain");
                !drained.has_value())
            {
                return drained;
            }
            kernel_alive = false;
            return {};
        }
    }
    if (Status slept = clock.sleep(100, cancellation); !slept.has_value())
    {
        return slept;
    }
    if (Status drained = drain(transport, cancellation, 100, "exhausted WRX drain");
        !drained.has_value())
    {
        return drained;
    }
    return fail(ErrorKind::Timeout, "no response from bootloader");
}

Status SubaruDensoSh7055_02Executor::upload_kernel(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events, const KernelImage& kernel)
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

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:267-295:
    // pad to a four-byte boundary, XOR 0x55/add 0x10, then wrap the bytes in
    // the fixed 0x31/0x61 SSM-shaped upload envelope with two checksums.
    bytes::Bytes encrypted = kernel.bytes;
    while (encrypted.size() % 4 != 0)
    {
        encrypted.push_back(0x00);
    }
    for (bytes::Byte& byte : encrypted)
    {
        byte = static_cast<bytes::Byte>((byte ^ 0x55) + 0x10);
    }
    const std::uint32_t address = kernel.load_address;
    const std::uint32_t length = static_cast<std::uint32_t>(encrypted.size() + 4);
    bytes::Bytes request{kOpUploadKernel,
                         static_cast<bytes::Byte>((address >> 16) & 0xFF),
                         static_cast<bytes::Byte>((address >> 8) & 0xFF)};
    bytes::appendU24Be(request, length);
    request.push_back(static_cast<bytes::Byte>((0x00 ^ 0x55) + 0x10));
    request.push_back(0x00);
    request.push_back(0x31);
    request.push_back(0x61);
    request[7] = fastecu::checksum::checksum8(request, true);
    request.insert(request.end(), encrypted.begin(), encrypted.end());
    request.push_back(fastecu::checksum::checksum8(request, true));

    events.log(LogLevel::Info, "Sending kernel...");
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:297-321.
    Result<IKlineFlashTransport::OptionalBytes> upload_response =
        exchange(transport, nullptr, cancellation, request, 0, 200);
    if (!upload_response.has_value())
    {
        return std::unexpected(upload_response.error());
    }
    if (upload_response->has_value() && !(**upload_response).empty())
    {
        return fail(ErrorKind::BadResponse, "error on kernel upload");
    }
    events.log(LogLevel::Info, "Kernel uploaded successfully");

    if (Status baud = change_baud(transport, cancellation, 62500); !baud.has_value())
    {
        return baud;
    }
    if (Status slept = clock.sleep(100, cancellation); !slept.has_value())
    {
        return slept;
    }
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:323-352 and 1169-1198.
    Result<bytes::Bytes> id = request_kernel_id(transport, clock, cancellation);
    if (!id.has_value())
    {
        return std::unexpected(id.error());
    }
    if (id->size() <= 4)
    {
        return fail(ErrorKind::BadResponse, "no valid response from ECU after kernel upload");
    }
    if (!response_ok(*id, static_cast<bytes::Byte>(kOpId | 0x40)))
    {
        return fail(ErrorKind::BadResponse, "wrong response from ECU after kernel upload");
    }
    events.log(LogLevel::Info, "Kernel is alive");
    return {};
}

Result<FlashExecutionResult> SubaruDensoSh7055_02Executor::execute(
    const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events)
{
    if (Status match = check_family_transport_match(plan, FlashFamily::SubaruDensoSh7055_02,
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
    const auto& family_plan = std::get<SubaruDensoSh7055_02Plan>(plan.family_plan());
    IKlineFlashTransport& kline = *kline_ptr;

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:57-70.
    if (Status cancelled = check_cancelled(cancellation, "cancelled before transport configuration");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (Status configured = kline.configure(KlineConfig{.baud = 62500,
                                                        .iso14230 = false,
                                                        .tester_id = family_plan.tester_id,
                                                        .target_id = family_plan.target_id});
        !configured.has_value())
    {
        return std::unexpected(configured.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after transport configuration");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before opening transport");
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
        if (Status drained = drain(kline, cancellation, 10, "initial drain"); !drained.has_value())
        {
            return std::unexpected(drained.error());
        }

        bool kernel_alive = false;
        std::optional<std::string> ecu_id;
        if (Status connected = connect_bootloader(kline, clock, cancellation, events, family_plan,
                                                  family_plan.read_ecu_id, kernel_alive, ecu_id);
            !connected.has_value())
        {
            return std::unexpected(connected.error());
        }
        if (!kernel_alive)
        {
            if (!plan.kernel().has_value())
            {
                return fail(ErrorKind::InvalidConfig, "SH7055_02 requires a kernel image");
            }
            if (Status uploaded = upload_kernel(kline, clock, cancellation, events, *plan.kernel());
                !uploaded.has_value())
            {
                return std::unexpected(uploaded.error());
            }
        }

        // Task 8 adds read_mem and returns ecu_id as FlashExecutionResult::rom_id;
        // Task 9 adds write_mem. Connect/upload have completed at this boundary.
        return fail(ErrorKind::Unsupported,
                    plan.operation() == FlashOperation::Read
                        ? "SH7055_02 read phase not yet implemented"
                        : "SH7055_02 write phase not yet implemented");
    }();

    // Close is unconditional and intentionally ignores cancellation so a
    // future successful read/write phase can never leak an open transport.
    Status close_status = kline.close();
    if (phase_result.has_value())
    {
        if (!close_status.has_value())
        {
            return std::unexpected(close_status.error());
        }
        return std::move(*phase_result);
    }
    if (!close_status.has_value())
    {
        events.log(LogLevel::Warning, "close failed after SH7055_02 phase error");
    }
    return std::unexpected(phase_result.error());
}

} // namespace fastecu::flash
