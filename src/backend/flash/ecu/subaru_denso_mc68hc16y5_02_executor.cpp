#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{
namespace
{

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
// Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:339.
constexpr std::uint32_t kReadPageSize = 0x400;
// Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:937-948.
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
    return received.size() > 5 &&
           received[0] == static_cast<bytes::Byte>((kStartComm >> 8) & 0xFF) &&
           received[1] == static_cast<bytes::Byte>(kStartComm & 0xFF) &&
           received[4] == expected_opcode_with_ack;
}

// Shared cancellation-aware write/read exchange, porting legacy
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:116-119 and 269-270.
Result<bytes::Bytes> exchange_impl(IKlineFlashTransport& transport, IClock *clock,
                                   const ICancellationToken& cancellation,
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

Result<bytes::Bytes> exchange(IKlineFlashTransport& transport, IClock& clock,
                              const ICancellationToken& cancellation, bytes::ByteView request,
                              int settle_ms, int timeout_ms)
{
    return exchange_impl(transport, &clock, cancellation, request, settle_ms, timeout_ms);
}

Result<bytes::Bytes> exchange(IKlineFlashTransport& transport,
                              const ICancellationToken& cancellation, bytes::ByteView request,
                              int timeout_ms)
{
    return exchange_impl(transport, nullptr, cancellation, request, 0, timeout_ms);
}

Status drain_response(IKlineFlashTransport& transport,
                      const ICancellationToken& cancellation, int timeout_ms,
                      std::string detail)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before " + detail);
        !cancelled.has_value())
    {
        return cancelled;
    }
    Result<IKlineFlashTransport::OptionalBytes> drained =
        transport.read(timeout_ms, cancellation);
    if (!drained.has_value())
    {
        return std::unexpected(drained.error());
    }
    return check_cancelled(cancellation, "cancelled after " + detail);
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

// Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:69-70:
// discard any stale serial bytes after disabling LEC lines and before the
// boot-entry sequence. Empty and non-empty reads are both successful drains.
Status drain_initial_response(IKlineFlashTransport& transport, const ICancellationToken& cancellation)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before initial drain");
        !cancelled.has_value())
    {
        return cancelled;
    }
    auto drained = transport.read(10, cancellation);
    if (!drained.has_value())
    {
        return std::unexpected(drained.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after initial drain");
        !cancelled.has_value())
    {
        return cancelled;
    }
    return {};
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

Result<bytes::Bytes> SubaruDensoMc68hc16y5_02Executor::read_mem(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events, const MemoryRegion& region)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:323-455.
    // Its RAM-mapped unreadable-region fill (370-380) applies only to the
    // legacy zero-address/zero-length shorthand. Plans always provide the
    // explicit flash-only transfer region, so no portable equivalent is needed.
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
        const bytes::Bytes payload{
            0x00,
            static_cast<bytes::Byte>((address >> 16) & 0xFF),
            static_cast<bytes::Byte>((address >> 8) & 0xFF),
            static_cast<bytes::Byte>(address & 0xFF),
            static_cast<bytes::Byte>((kReadPageSize >> 8) & 0xFF),
            static_cast<bytes::Byte>(kReadPageSize & 0xFF),
        };
        Result<bytes::Bytes> response =
            exchange(transport, clock, cancellation, frame(kOpReadArea, payload), 10, 3000);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (!response_ok(*response, static_cast<bytes::Byte>(kOpReadArea | 0x40)))
        {
            return fail(ErrorKind::BadResponse, "Wrong response from ECU during read");
        }
        mapdata.insert(mapdata.end(), response->begin() + 5, response->end() - 1);
        address += kReadPageSize;
        --remaining_pages;
        events.progress(static_cast<int>(mapdata.size()), static_cast<int>(region.length));
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

Result<std::uint32_t> SubaruDensoMc68hc16y5_02Executor::read_block_crc(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    const MemoryRegion& block)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:626-715:
    // request the ECU's CRC over [start, start + length).
    bytes::Bytes payload{
        static_cast<bytes::Byte>((block.start >> 24) & 0xFF),
        static_cast<bytes::Byte>((block.start >> 16) & 0xFF),
        static_cast<bytes::Byte>((block.start >> 8) & 0xFF),
        static_cast<bytes::Byte>(block.start & 0xFF),
        0x00,
        static_cast<bytes::Byte>((block.length >> 16) & 0xFF),
        static_cast<bytes::Byte>((block.length >> 8) & 0xFF),
        static_cast<bytes::Byte>(block.length & 0xFF),
    };
    Result<bytes::Bytes> response =
        exchange(transport, cancellation, frame(kOpCrc, payload), 3000);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:660-667.
    for (int try_count = 0; response->size() < 10 && try_count < 20; ++try_count)
    {
        if (Status cancelled = check_cancelled(cancellation, "cancelled before CRC continuation read");
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
            const std::size_t needed = 10 - response->size();
            const std::size_t append_count = std::min(needed, (**more).size());
            response->insert(response->end(), (**more).begin(),
                             (**more).begin() + append_count);
        }
        if (Status slept = clock.sleep(100, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    if (response->size() <= 9 || !response_ok(*response, kOpCrc | 0x40))
    {
        return fail(ErrorKind::BadResponse, "Wrong response from ECU during CRC check");
    }
    const std::uint32_t crc = (static_cast<std::uint32_t>((*response)[5]) << 24) |
                              (static_cast<std::uint32_t>((*response)[6]) << 16) |
                              (static_cast<std::uint32_t>((*response)[7]) << 8) |
                              static_cast<std::uint32_t>((*response)[8]);
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:702-714.
    if (Status drained = drain_response(transport, cancellation, 200, "CRC response drain");
        !drained.has_value())
    {
        return std::unexpected(drained.error());
    }
    return crc;
}

Status SubaruDensoMc68hc16y5_02Executor::flash_block(
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
        // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:950-992.
        events.log(LogLevel::Info, "Erasing flash page...");
        bytes::Bytes erase_payload{
            static_cast<bytes::Byte>((block.start >> 24) & 0xFF),
            static_cast<bytes::Byte>((block.start >> 16) & 0xFF),
            static_cast<bytes::Byte>((block.start >> 8) & 0xFF),
            static_cast<bytes::Byte>(block.start & 0xFF),
        };
        // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:950-969.
        Result<bytes::Bytes> erase_response = exchange(
            transport, clock, cancellation, frame(kOpBlankPage, erase_payload), 500, 3000);
        if (!erase_response.has_value())
        {
            return std::unexpected(erase_response.error());
        }
        if (!response_ok(*erase_response, kOpBlankPage | 0x40))
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

        // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:994-1039.
        const std::uint32_t chunk_address = block.start + offset;
        bytes::Bytes payload{
            static_cast<bytes::Byte>((chunk_address >> 24) & 0xFF),
            static_cast<bytes::Byte>((chunk_address >> 16) & 0xFF),
            static_cast<bytes::Byte>((chunk_address >> 8) & 0xFF),
            static_cast<bytes::Byte>(chunk_address & 0xFF),
        };
        payload.insert(payload.end(), image.begin() + chunk_address,
                       image.begin() + chunk_address + kWriteChunkSize);
        Result<bytes::Bytes> response =
            exchange(transport, cancellation, frame(kOpWriteFlashBuffer, payload), 3000);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (!response_ok(*response, kOpWriteFlashBuffer | 0x40))
        {
            return fail(ErrorKind::BadResponse, "Wrong response from ECU during write");
        }
        offset += kWriteChunkSize;

        if (commit_block_start + kCommitBlockSize == block.start + offset)
        {
            // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1072-1128.
            const std::uint32_t commit_crc = fastecu::checksum::crc32(
                image.subspan(commit_block_start, kCommitBlockSize));
            const std::uint8_t commit_opcode =
                test_write ? kOpValidateFlashBuffer : kOpCommitFlashBuffer;
            bytes::Bytes commit_payload{
                static_cast<bytes::Byte>((commit_block_start >> 24) & 0xFF),
                static_cast<bytes::Byte>((commit_block_start >> 16) & 0xFF),
                static_cast<bytes::Byte>((commit_block_start >> 8) & 0xFF),
                static_cast<bytes::Byte>(commit_block_start & 0xFF),
                static_cast<bytes::Byte>((kCommitBlockSize >> 8) & 0xFF),
                static_cast<bytes::Byte>(kCommitBlockSize & 0xFF),
                static_cast<bytes::Byte>((commit_crc >> 24) & 0xFF),
                static_cast<bytes::Byte>((commit_crc >> 16) & 0xFF),
                static_cast<bytes::Byte>((commit_crc >> 8) & 0xFF),
                static_cast<bytes::Byte>(commit_crc & 0xFF),
            };
            Result<bytes::Bytes> commit_response = exchange(
                transport, clock, cancellation, frame(commit_opcode, commit_payload), 200, 3000);
            if (!commit_response.has_value())
            {
                return std::unexpected(commit_response.error());
            }
            if (!response_ok(*commit_response, commit_opcode | 0x40))
            {
                return fail(ErrorKind::BadResponse, "Wrong response from ECU during commit");
            }
            commit_block_start += kCommitBlockSize;
        }
        events.progress(static_cast<int>(offset), static_cast<int>(block.length));
    }
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1129-1133.
    if (Status drained = drain_response(transport, cancellation, 200, "flash block drain");
        !drained.has_value())
    {
        return drained;
    }
    return {};
}

Status SubaruDensoMc68hc16y5_02Executor::write_mem(
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
    if (image.size() != device->romsize || physical_size > std::numeric_limits<std::size_t>::max())
    {
        return fail(ErrorKind::InvalidConfig, "ROM image does not match the flash device");
    }

    // Task 2 accepts the packed 160 KiB ROM. The legacy write path padded the
    // 0x20000-0x27fff RAM/kernel hole before indexing by physical address
    // (src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:460-489).
    bytes::Bytes addressed_image(static_cast<std::size_t>(physical_size), 0xFF);
    std::size_t image_offset = 0;
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& flash_block = device->fblocks[block_no];
        if (flash_block.len > image.size() - image_offset)
        {
            return fail(ErrorKind::InvalidConfig, "ROM image is shorter than its flash blocks");
        }
        std::copy_n(image.begin() + image_offset, flash_block.len,
                    addressed_image.begin() + flash_block.start);
        image_offset += flash_block.len;
    }
    if (image_offset != image.size())
    {
        return fail(ErrorKind::InvalidConfig, "ROM image is longer than its flash blocks");
    }

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:460-620.
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
                bytes::ByteView(addressed_image).subspan(block.start, block.length));
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

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:501-515.
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

    // Legacy init_flash_write(), full exchanges at
    // src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:717-838.
    for (const std::uint8_t opcode : {kOpGetMaxMsgSize, kOpGetMaxBlockSize})
    {
        Result<bytes::Bytes> response =
            exchange(transport, clock, cancellation, frame(opcode), 200, 200);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (response->size() <= 9 || !response_ok(*response, opcode | 0x40))
        {
            return fail(ErrorKind::BadResponse, "Wrong response from ECU during flash init");
        }
    }
    const std::uint8_t enable_opcode = test_write ? kOpFlashDisable : kOpFlashEnable;
    Result<bytes::Bytes> enable_response =
        exchange(transport, clock, cancellation, frame(enable_opcode), 200, 200);
    if (!enable_response.has_value())
    {
        return std::unexpected(enable_response.error());
    }
    if (!response_ok(*enable_response, enable_opcode | 0x40))
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
        // Legacy reflash_block(), full PROG_VOLT exchange at
        // src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:845-921.
        Result<bytes::Bytes> voltage_response =
            exchange(transport, cancellation, frame(kOpProgVolt), 200);
        if (!voltage_response.has_value())
        {
            return std::unexpected(voltage_response.error());
        }
        if (voltage_response->size() <= 7 ||
            !response_ok(*voltage_response, kOpProgVolt | 0x40))
        {
            return fail(ErrorKind::BadResponse, "Wrong response from ECU during prog-volt query");
        }
        if (Status flashed = flash_block(transport, clock, cancellation, events,
                                         addressed_image, block, test_write);
            !flashed.has_value())
        {
            return flashed;
        }
        events.log(LogLevel::Info, "Block reflash complete");
    }

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:545-579.
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
        if (Status drained = drain_initial_response(kline, cancellation); !drained.has_value())
        {
            return std::unexpected(drained.error());
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
        if (plan.operation() == FlashOperation::Read)
        {
            Result<bytes::Bytes> read =
                read_mem(kline, clock, cancellation, events, plan.transfer_region());
            if (!read.has_value())
            {
                return std::unexpected(read.error());
            }
            return FlashExecutionResult{.operation = plan.operation(), .read_bytes = std::move(*read)};
        }

        if (!plan.image().has_value())
        {
            return fail(ErrorKind::InvalidConfig, "MC68HC16Y5_02 write requires a ROM image");
        }
        Status written = write_mem(kline, clock, cancellation, events, *plan.image(),
                                   plan.mcu_name(),
                                   plan.operation() == FlashOperation::TestWrite);
        if (!written.has_value())
        {
            return std::unexpected(written.error());
        }
        return FlashExecutionResult{.operation = plan.operation(), .read_bytes = std::nullopt};
    }();

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
        events.log(LogLevel::Warning, "close failed after MC68HC16Y5_02 phase error");
    }
    return std::unexpected(phase_result.error());
}

} // namespace fastecu::flash
