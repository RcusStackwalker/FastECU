#include "src/backend/flash/ecu/subaru_denso_sh7055_02_executor.h"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{
namespace
{
using namespace bytes;
using namespace bytes::literals;

constexpr std::uint16_t kStartComm = 0xBEEF;
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
constexpr std::uint8_t kOpUploadKernel = 0x53;
// Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:372-377.
constexpr std::uint32_t kReadPageSize = 0x400;
// Legacy flash_block(), lines 969 and 980.
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitBlockSize = 0x1000;

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
// [0xBE][0xEF][length high][length low][opcode][payload][sum8].
bytes::Bytes frame(std::uint8_t opcode, bytes::ByteView payload = {})
{
    const std::uint16_t length = static_cast<std::uint16_t>(payload.size() + 1);
    return composeBeWithChecksum(bytes::sum8, kStartComm, length, bytes::Byte(opcode), payload);
}

bool response_ok(bytes::ByteView received, std::uint8_t expected_opcode)
{
    return received.size() > 5 && received[0] == 0xBE && received[1] == 0xEF &&
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
    if (Status cancelled = check_cancelled(cancellation, std::format("cancelled before {}", detail));
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (const auto drained = transport.read(timeout_ms, cancellation); !drained.has_value())
    {
        return std::unexpected(drained.error());
    }
    return check_cancelled(cancellation, std::format("cancelled after {}", detail));
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
            bytes::Bytes{0xBF}, family_plan.tester_id, family_plan.target_id);
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
        events.log(LogLevel::Info, std::format("Starting in {}", seconds_left));
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
        const bool matches_expected = received->has_value() && (**received).size() == 3 &&
                                      (**received)[0] == 0x4D && (**received)[1] == 0x00 &&
                                      (**received)[2] == 0xB3;
        // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:201-224
        // and 1276-1292: STATUS_SUCCESS is zero only for an exact match, so
        // !check_received_message(...) enters the connected branch.
        if (matches_expected)
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
    // Not a full u24(address): the frame carries only the address's high two
    // bytes here (bits 23-8). The low byte is never emitted in this header --
    // bytes 6-9 below are a fixed transform of the literal 0x00, the checksum
    // placeholder overwritten at request[7], and the fixed 0x31/0x61 envelope
    // markers, none of which reference address.
    bytes::Bytes request = composeBe(kOpUploadKernel, std::uint16_t(address >> 8));
    bytes::appendU24Be(request, length);
    request.push_back(static_cast<bytes::Byte>((0x00 ^ 0x55) + 0x10));
    request.push_back(0x00);
    request.push_back(0x31);
    request.push_back(0x61);
    // Not composeBeWithChecksum: the first checksum is patched into the
    // middle of the frame at offset 7, and the second covers the frame plus
    // the encrypted payload appended afterwards.
    request[7] = fastecu::checksum::negatedSum8(request);
    request.insert(request.end(), encrypted.begin(), encrypted.end());
    request.push_back(fastecu::checksum::negatedSum8(request));

    events.log(LogLevel::Info, "Sending kernel...");
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:297-321.
    // On Unix the OpenPort2/J2534 adapter requires the legacy 5000 ms quiet
    // period after the raw write and before reading its upload response.
    // IClock keeps that wait deterministic and cancellation-aware.
    const int post_upload_delay_ms =
        transport.requires_post_kernel_upload_delay() ? 5000 : 0;
    Result<IKlineFlashTransport::OptionalBytes> upload_response =
        exchange(transport, &clock, cancellation, request, post_upload_delay_ms, 200);
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

Result<bytes::Bytes> SubaruDensoSh7055_02Executor::read_mem(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events, const MemoryRegion& region)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:360-455:
    // request consecutive 0x400-byte pages with SUB_KERNEL_READ_AREA, remove
    // the five-byte response envelope and its trailing checksum, then retain
    // the received bytes. SH7055_02 has no rblocks skip branch in this path.
    bytes::Bytes mapdata;
    std::uint32_t address = region.start;
    std::uint32_t remaining_pages = (region.length + kReadPageSize - 1) / kReadPageSize;
    events.progress(0, static_cast<int>(region.length));

    while (remaining_pages > 0)
    {
        if (Status cancelled = check_cancelled(cancellation, "cancelled during read");
            !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        const bytes::Bytes payload =
            composeBe(0x00_b, u24(address), std::uint16_t(kReadPageSize));
        // Legacy lines 415-421 settle for 10ms and use serial_read_extra_long_timeout (3000ms).
        Result<IKlineFlashTransport::OptionalBytes> response =
            exchange(transport, &clock, cancellation, frame(kOpReadArea, payload), 10, 3000);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        // Legacy lines 423-437 only accept the BEEF / 0x43 response envelope
        // before removing its fixed header and final checksum. Its fixed page
        // request (lines 415-416) requires a full 0x400-byte payload; reject
        // short replies rather than returning a silently truncated ROM.
        if (!response->has_value() ||
            !response_ok(**response, static_cast<bytes::Byte>(kOpReadArea | 0x40)) ||
            (**response).size() != kReadPageSize + 6)
        {
            return fail(ErrorKind::BadResponse, "Wrong response from ECU during read");
        }
        const bytes::Bytes& received = **response;
        mapdata.insert(mapdata.end(), received.begin() + 5, received.end() - 1);
        address += kReadPageSize;
        --remaining_pages;
        events.progress(static_cast<int>(mapdata.size()), static_cast<int>(region.length));
        // Legacy line 466 paces successive requests by 1ms.
        if (Status slept = clock.sleep(1, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    if (mapdata.size() > region.length)
    {
        mapdata.resize(region.length);
    }
    events.progress(static_cast<int>(region.length), static_cast<int>(region.length));
    return mapdata;
}

Result<std::uint32_t> SubaruDensoSh7055_02Executor::read_block_crc(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    const MemoryRegion& block)
{
    // Legacy check_romcrc(), lines 645-749: request the ECU CRC for the
    // physical [start, start + length) block.
    const bytes::Bytes payload = composeBe(block.start, 0x00_b, u24(block.length));
    const bytes::Bytes request = frame(kOpCrc, payload);
    if (Status cancelled = check_cancelled(cancellation, "cancelled before CRC write");
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
    if (Status cancelled = check_cancelled(cancellation, "cancelled after CRC write");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before initial CRC read");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    Result<IKlineFlashTransport::OptionalBytes> initial = transport.read(3000, cancellation);
    if (!initial.has_value())
    {
        return std::unexpected(initial.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after initial CRC read");
        !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    bytes::Bytes response = initial->has_value() ? std::move(**initial) : bytes::Bytes{};

    // Legacy lines 680-686 accumulate fragmented replies using twenty 50ms
    // reads paced by 100ms. Use the BEEF frame's declared length once its
    // four-byte header is present: SH success carries opcode + one prefix +
    // four CRC bytes, so its complete frame is 11 bytes, not MC68's 10.
    const auto declared_frame_size = [&response]() -> std::optional<std::size_t>
    {
        if (response.size() < 4)
        {
            return std::nullopt;
        }
        const std::size_t declared =
            (static_cast<std::size_t>(response[2]) << 8) | response[3];
        // This phase accepts only opcode plus the one-byte marker/prefix and
        // four CRC bytes. Bound the declared size before accumulating.
        return declared <= 6 ? std::optional<std::size_t>{declared + 5} : std::optional<std::size_t>{0};
    };
    int try_count = 0;
    while (try_count < 20)
    {
        const std::optional<std::size_t> expected_size = declared_frame_size();
        if (expected_size.has_value() && *expected_size == 0)
        {
            return fail(ErrorKind::BadResponse, "Oversized CRC response from ECU");
        }
        if (expected_size.has_value() && response.size() >= *expected_size)
        {
            break;
        }
        if (Status cancelled = check_cancelled(cancellation,
                                               "cancelled before CRC continuation read");
            !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        Result<IKlineFlashTransport::OptionalBytes> more = transport.read(50, cancellation);
        if (!more.has_value())
        {
            return std::unexpected(more.error());
        }
        if (more->has_value())
        {
            response.insert(response.end(), (**more).begin(), (**more).end());
        }
        if (Status cancelled = check_cancelled(cancellation,
                                               "cancelled after CRC continuation read");
            !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        if (Status slept = clock.sleep(100, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
        ++try_count;
    }
    const std::optional<std::size_t> expected_size = declared_frame_size();
    if (!expected_size.has_value() || *expected_size == 0 || response.size() != *expected_size ||
        response.size() < 7 || !response_ok(response, kOpCrc | 0x40) ||
        response.back() != bytes::sum8(bytes::ByteView(response).first(response.size() - 1)))
    {
        return fail(ErrorKind::BadResponse, "Wrong or incomplete response from ECU during CRC check");
    }

    // SH7055-specific legacy unwrap, lines 704-717: remove the five-byte
    // BEEF/opcode envelope and checksum, then inspect and strip the payload's
    // length/failure prefix. MC68 has neither this payload prefix nor marker.
    response.erase(response.begin(), response.begin() + 5);
    response.pop_back();
    if (response.empty())
    {
        return fail(ErrorKind::BadResponse, "Missing CRC response prefix");
    }
    const bytes::Byte length_or_failure = response.front();
    if (length_or_failure == 0x7F)
    {
        return fail(ErrorKind::BadResponse, "ECU marked CRC response failed");
    }
    response.erase(response.begin());
    if (response.size() != 4)
    {
        return fail(ErrorKind::BadResponse, "Truncated CRC response from ECU");
    }
    const std::uint32_t crc = (static_cast<std::uint32_t>(response[0]) << 24) |
                              (static_cast<std::uint32_t>(response[1]) << 16) |
                              (static_cast<std::uint32_t>(response[2]) << 8) |
                              static_cast<std::uint32_t>(response[3]);
    // Legacy lines 742 and 748 perform a short read after either compare
    // outcome, so drain every successful CRC decode before returning it.
    if (Status drained = drain(transport, cancellation, 200, "CRC response drain");
        !drained.has_value())
    {
        return std::unexpected(drained.error());
    }
    return crc;
}

Status SubaruDensoSh7055_02Executor::flash_block(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events, bytes::ByteView image, const MemoryRegion& block, bool test_write)
{
    if (block.start > image.size() || block.length > image.size() - block.start ||
        block.length % kWriteChunkSize != 0 || block.length % kCommitBlockSize != 0)
    {
        return fail(ErrorKind::InvalidConfig, "flash block is not represented by the image");
    }

    if (!test_write)
    {
        // Legacy flash_block(), lines 982-1019: erase, settle 500ms, then
        // wait up to 3000ms for the 0x65 acknowledgment.
        events.log(LogLevel::Info, "Erasing flash page...");
        const bytes::Bytes erase_payload = composeBe(block.start);
        Result<IKlineFlashTransport::OptionalBytes> erase_exchange = exchange(
            transport, &clock, cancellation, frame(kOpBlankPage, erase_payload), 500, 3000);
        if (!erase_exchange.has_value())
        {
            return std::unexpected(erase_exchange.error());
        }
        if (!erase_exchange->has_value())
        {
            return fail(ErrorKind::Timeout, "no response from ECU during erase");
        }
        if (!response_ok(**erase_exchange, kOpBlankPage | 0x40))
        {
            return fail(ErrorKind::BadResponse, "Wrong response from ECU during erase");
        }
        events.log(LogLevel::Info, "Erased");
    }

    std::uint32_t offset = 0;
    std::uint32_t commit_block_start = block.start;
    while (offset < block.length)
    {
        if (Status cancelled = check_cancelled(cancellation, "cancelled during block write");
            !cancelled.has_value())
        {
            return cancelled;
        }
        const std::uint32_t chunk_address = block.start + offset;
        const bytes::Bytes payload =
            composeBe(chunk_address, image.subspan(chunk_address, kWriteChunkSize));

        // SH7055 legacy lines 1048-1050 actively settle 50ms and use the
        // normal 2000ms timeout; MC68's corresponding delay is commented.
        Result<IKlineFlashTransport::OptionalBytes> write_response = exchange(
            transport, &clock, cancellation, frame(kOpWriteFlashBuffer, payload), 50, 2000);
        if (!write_response.has_value())
        {
            return std::unexpected(write_response.error());
        }
        if (!write_response->has_value())
        {
            return fail(ErrorKind::Timeout, "no response from ECU during write");
        }
        if (!response_ok(**write_response, kOpWriteFlashBuffer | 0x40))
        {
            return fail(ErrorKind::BadResponse, "Wrong response from ECU during write");
        }
        offset += kWriteChunkSize;

        if (commit_block_start + kCommitBlockSize == block.start + offset)
        {
            const std::uint32_t commit_crc = fastecu::checksum::crc32(
                image.subspan(commit_block_start, kCommitBlockSize));
            const std::uint8_t commit_opcode =
                test_write ? kOpValidateFlashBuffer : kOpCommitFlashBuffer;
            const bytes::Bytes commit_payload =
                composeBe(commit_block_start, std::uint16_t(kCommitBlockSize), commit_crc);
            Result<IKlineFlashTransport::OptionalBytes> commit_response = exchange(
                transport, &clock, cancellation, frame(commit_opcode, commit_payload), 200, 3000);
            if (!commit_response.has_value())
            {
                return std::unexpected(commit_response.error());
            }
            if (!commit_response->has_value())
            {
                return fail(ErrorKind::Timeout, "no response from ECU during commit");
            }
            if (!response_ok(**commit_response, commit_opcode | 0x40))
            {
                return fail(ErrorKind::BadResponse, "Wrong response from ECU during commit");
            }
            commit_block_start += kCommitBlockSize;
        }
        events.progress(static_cast<int>(offset), static_cast<int>(block.length));
    }
    // Legacy line 1159 discards one final short-timeout response.
    return drain(transport, cancellation, 200, "flash block drain");
}

Status SubaruDensoSh7055_02Executor::write_mem(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events, bytes::ByteView image, const std::string& mcu_name, bool test_write)
{
    const flashdev_t *device = find_flash_device(mcu_name);
    if (device == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "Unknown MCU type");
    }
    std::uint64_t physical_size = 0;
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        physical_size = std::max(
            physical_size, static_cast<std::uint64_t>(device->fblocks[block_no].start) +
                               device->fblocks[block_no].len);
    }
    if (image.size() != device->romsize || physical_size > image.size() ||
        physical_size > std::numeric_limits<std::size_t>::max())
    {
        return fail(ErrorKind::InvalidConfig, "ROM image does not match the flash device");
    }

    // Legacy write_mem()/get_changed_blocks(), lines 484-637.
    events.log(LogLevel::Info, "Comparing ECU flash memory pages to image file");
    const auto compare_blocks = [&](std::vector<bool>& modified) -> Result<unsigned>
    {
        unsigned modified_count = 0;
        for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
        {
            if (Status cancelled = check_cancelled(cancellation, "cancelled during ROM compare");
                !cancelled.has_value())
            {
                return std::unexpected(cancelled.error());
            }
            const MemoryRegion block{device->fblocks[block_no].start,
                                     device->fblocks[block_no].len};
            Result<std::uint32_t> ecu_crc =
                read_block_crc(transport, clock, cancellation, block);
            if (!ecu_crc.has_value())
            {
                return std::unexpected(ecu_crc.error());
            }
            const std::uint32_t image_crc = fastecu::checksum::crc32(
                image.subspan(block.start, block.length));
            modified[block_no] = *ecu_crc != image_crc;
            modified_count += modified[block_no] ? 1u : 0u;
        }
        return modified_count;
    };

    std::vector<bool> modified(device->numblocks, false);
    Result<unsigned> modified_count = compare_blocks(modified);
    if (!modified_count.has_value())
    {
        return std::unexpected(modified_count.error());
    }

    // Legacy line 528 transitions the serial control lines after compare,
    // including the no-difference path.
    if (Status cancelled = check_cancelled(cancellation, "cancelled before programming line state");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status line = transport.enable_programming_voltage_line(); !line.has_value())
    {
        return line;
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after programming line state");
        !cancelled.has_value())
    {
        return cancelled;
    }

    if (*modified_count == 0)
    {
        events.log(LogLevel::Info,
                   "No difference between ROM and ECU data, no flashing needed");
        return {};
    }

    // Legacy init_flash_write(), lines 752-873. SH7055's returned 32-bit
    // lengths begin at offsets 6..9, one byte later than MC68.
    for (const std::uint8_t opcode : {kOpGetMaxMsgSize, kOpGetMaxBlockSize})
    {
        Result<IKlineFlashTransport::OptionalBytes> response =
            exchange(transport, &clock, cancellation, frame(opcode), 200, 200);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (!response->has_value())
        {
            return fail(ErrorKind::Timeout, "no response from ECU during flash init");
        }
        if ((**response).size() <= 9 || !response_ok(**response, opcode | 0x40))
        {
            return fail(ErrorKind::BadResponse, "Wrong response from ECU during flash init");
        }
        const bytes::Bytes& received = **response;
        const std::uint32_t length = (static_cast<std::uint32_t>(received[6]) << 24) |
                                     (static_cast<std::uint32_t>(received[7]) << 16) |
                                     (static_cast<std::uint32_t>(received[8]) << 8) |
                                     static_cast<std::uint32_t>(received[9]);
        if (opcode == kOpGetMaxMsgSize)
        {
            events.log(LogLevel::Info, std::format("Max message length: 0x{:08X}", length));
        }
        else
        {
            events.log(LogLevel::Info, std::format("Flash block size: 0x{:08X}", length));
        }
    }
    const std::uint8_t enable_opcode = test_write ? kOpFlashDisable : kOpFlashEnable;
    Result<IKlineFlashTransport::OptionalBytes> enable_response =
        exchange(transport, &clock, cancellation, frame(enable_opcode), 200, 200);
    if (!enable_response.has_value())
    {
        return std::unexpected(enable_response.error());
    }
    if (!enable_response->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from ECU during flash init");
    }
    if (!response_ok(**enable_response, enable_opcode | 0x40))
    {
        return fail(ErrorKind::BadResponse, "Wrong response from ECU during flash init");
    }

    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        if (!modified[block_no])
        {
            continue;
        }
        const MemoryRegion block{device->fblocks[block_no].start,
                                 device->fblocks[block_no].len};
        // Legacy reflash_block(), lines 914-944: no settle before the 200ms
        // programming-voltage reply, whose two-byte voltage requires >7 bytes.
        Result<IKlineFlashTransport::OptionalBytes> voltage_response =
            exchange(transport, nullptr, cancellation, frame(kOpProgVolt), 0, 200);
        if (!voltage_response.has_value())
        {
            return std::unexpected(voltage_response.error());
        }
        if (!voltage_response->has_value())
        {
            return fail(ErrorKind::Timeout, "no response from ECU during prog-volt query");
        }
        if ((**voltage_response).size() <= 7 ||
            !response_ok(**voltage_response, kOpProgVolt | 0x40))
        {
            return fail(ErrorKind::BadResponse, "Wrong response from ECU during prog-volt query");
        }
        if (Status flashed = flash_block(transport, clock, cancellation, events, image,
                                         block, test_write);
            !flashed.has_value())
        {
            return flashed;
        }
        events.log(LogLevel::Info, "Block reflash complete");
    }

    // Legacy lines 562-593 repeat the per-block CRC comparison after all
    // selected blocks have been transferred.
    events.log(LogLevel::Info, "Comparing ECU flash memory pages to image file after reflash");
    Result<unsigned> remaining_modified = compare_blocks(modified);
    if (!remaining_modified.has_value())
    {
        return std::unexpected(remaining_modified.error());
    }
    if (test_write)
    {
        events.log(LogLevel::Info,
                   "Test write PASS, it is safe to perform the actual write");
    }
    else if (*remaining_modified != 0)
    {
        events.log(LogLevel::Error,
                   "Flash verification differs; do not power off, the kernel is still running");
    }
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
    if (Status valid = validate_subaru_denso_sh7055_02_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
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

        if (plan.operation() == FlashOperation::Read)
        {
            Result<bytes::Bytes> read =
                read_mem(kline, clock, cancellation, events, plan.transfer_region());
            if (!read.has_value())
            {
                return std::unexpected(read.error());
            }
            return FlashExecutionResult{
                .operation = plan.operation(), .read_bytes = std::move(*read), .rom_id = ecu_id};
        }

        if (!plan.image().has_value())
        {
            return fail(ErrorKind::InvalidConfig, "SH7055_02 write requires a ROM image");
        }
        if (Status written = write_mem(kline, clock, cancellation, events, *plan.image(),
                                       plan.mcu_name(),
                                       plan.operation() == FlashOperation::TestWrite);
            !written.has_value())
        {
            return std::unexpected(written.error());
        }
        return FlashExecutionResult{.operation = plan.operation(), .read_bytes = std::nullopt};
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
