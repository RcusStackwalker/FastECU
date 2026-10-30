#include "src/backend/flash/ecu/subaru_denso_sh7055_02_executor.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <limits>
#include <utility>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/ecu/denso_beef_kline_common.h"
#include "src/backend/flash/ecu/plan_primitives.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/ports/cancellation.h"

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;

// Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:372-377.
constexpr std::uint32_t kReadPageSize = 0x400;
// Legacy flash_block(), lines 969 and 980.
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitBlockSize = 0x1000;

// Read-only drain used where legacy intentionally discarded stale/echo bytes.
// Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:69-70,
// 170-172, 194-195, 218-220, and 225-228.
Status Drain(IKlineFlashTransport& transport, const ICancellationToken& cancellation, std::chrono::milliseconds timeout,
             std::string_view detail)
{
    return DensoBeefKlineGuarded(cancellation, detail,
                                 [&]() -> Status
                                 {
                                     if (const auto drained = transport.Read(timeout, cancellation);
                                         !drained.has_value())
                                     {
                                         return std::unexpected(drained.error());
                                     }
                                     return {};
                                 });
}

// Kernel-ID exchange. Legacy
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:1169-1198.
Result<bytes::Bytes> RequestKernelId(IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation)
{
    const bytes::Bytes request = DensoBeefKlineFrame(kDensoKlineOpId);
    Result<IKlineFlashTransport::OptionalBytes> received =
        DensoBeefKlineExchangeOptional(transport, &clock, cancellation, request, 200ms, 2000ms);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    return received->has_value() ? std::move(**received) : bytes::Bytes{};
}

std::string EcuIdHex(bytes::ByteView id)
{
    std::string result;
    result.reserve(id.size() * 2);
    for (const bytes::Byte byte : id)
    {
        result += std::format("{:02X}", byte);
    }
    return result;
}

Status ChangeBaud(IKlineFlashTransport& transport, const ICancellationToken& cancellation, int baud)
{
    return DensoBeefKlineGuarded(cancellation, "changing baud", [&] { return transport.SetBaud(baud); });
}

// Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:133-168
// and 1206-1218.
Result<std::string> ReadEcuId(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                              IEventSink& events, const SubaruDensoSh7055_02Plan& family_plan)
{
    return ChangeBaud(transport, cancellation, 4800)
        .and_then(
            [&]
            {
                events.Log(LogLevel::kInfo, "Requesting ECU ID");
                const bytes::Bytes request =
                    ssm_protocol::AddHeader(bytes::Bytes{0xBF}, family_plan.tester_id, family_plan.target_id);
                return DensoBeefKlineExchangeOptional(transport, nullptr, cancellation, request, 0ms, 2000ms);
            })
        .and_then(
            [&](const IKlineFlashTransport::OptionalBytes& received) -> Result<std::string>
            {
                if (!received.has_value())
                {
                    return Fail(ErrorKind::kTimeout, "no response from ECU during ID read");
                }
                const bytes::Bytes& response = *received;
                if (response.size() <= 4 || response[4] != 0xFF)
                {
                    return Fail(ErrorKind::kBadResponse, "wrong response from ECU during ID read");
                }
                if (response.size() < 13)
                {
                    return Fail(ErrorKind::kBadResponse, "ECU ID response is too short");
                }
                std::string ecu_id = EcuIdHex(bytes::ByteView(response).subspan(8, 5));
                events.Log(LogLevel::kInfo, std::format("ECU ID: {}", ecu_id));
                return ecu_id;
            });
}

// The plan's upfront CycleIgnition confirmation replaces the legacy
// executor-side human prompt at
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:170-173.
// No prompt occurs here: a countdown precedes the boot-mode LEC pulse.
Status EnterBootMode(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                     IEventSink& events)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:170-172.
    if (Status drained =
            DensoBeefKlineGuarded(cancellation, "disabling LEC lines", [&] { return transport.DisableLecLines(); })
                .and_then([&] { return Drain(transport, cancellation, 10ms, "pre-countdown drain"); });
        !drained.has_value())
    {
        return drained;
    }
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:188-196 and
    // 1300-1314.
    for (int seconds_left = 3; seconds_left > 0; --seconds_left)
    {
        events.Log(LogLevel::kInfo, std::format("Starting in {}", seconds_left));
        if (Status slept = clock.Sleep(1000ms, cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    events.Log(LogLevel::kInfo, "Switch Ignition ON!");
    return clock.Sleep(250ms, cancellation)
        .and_then(
            [&]
            {
                return DensoBeefKlineGuarded(cancellation, "LEC pulse", [&] { return transport.PulseLec2Line(200ms); });
            })
        .and_then([&] { return Drain(transport, cancellation, 10ms, "post-pulse drain"); })
        .and_then([&] { return clock.Sleep(190ms, cancellation); });
}

// Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:198-228.
// True once the bootloader answers the WRX init exactly.
Result<bool> InitBootloader(IKlineFlashTransport& transport, const ICancellationToken& cancellation)
{
    const bytes::Bytes init_request{0x4D, 0xFF, 0xB4};
    const bytes::Bytes expected{0x4D, 0x00, 0xB3};
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (Status cancelled = CheckCancelled(cancellation, "cancelled during WRX init"); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        Result<IKlineFlashTransport::OptionalBytes> received =
            DensoBeefKlineExchangeOptional(transport, nullptr, cancellation, init_request, 0ms, 10ms);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:201-224
        // and 1276-1292: STATUS_SUCCESS is zero only for an exact match, so
        // !check_received_message(...) enters the connected branch.
        if (*received == expected)
        {
            return true;
        }
    }
    return false;
}

// Accumulates a CRC reply. Legacy lines 680-686 use twenty 50ms reads paced
// by 100ms. Use the BEEF frame's declared length once its four-byte header
// is present: SH success carries opcode + one prefix + four CRC bytes, so its
// complete frame is 11 bytes, not MC68's 10. This phase accepts only that
// payload, so a larger declared size reports 0 before accumulating.
std::optional<std::size_t> DeclaredCrcFrameSize(bytes::ByteView response)
{
    if (response.size() < 4)
    {
        return std::nullopt;
    }
    const std::size_t declared = bytes::ReadU16Be(response, 2);
    return declared <= 6 ? declared + 5 : 0;
}

Status ReadCrcContinuation(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                           bytes::Bytes& response)
{
    for (int try_count = 0; try_count < 20; ++try_count)
    {
        const std::optional<std::size_t> expected_size = DeclaredCrcFrameSize(response);
        if (expected_size == 0)
        {
            return Fail(ErrorKind::kBadResponse, "Oversized CRC response from ECU");
        }
        if (expected_size.has_value() && response.size() >= *expected_size)
        {
            return {};
        }
        if (Status read = DensoBeefKlineGuarded(cancellation, "CRC continuation read",
                                                [&]() -> Status
                                                {
                                                    auto more = transport.Read(50ms, cancellation);
                                                    if (!more.has_value())
                                                    {
                                                        return std::unexpected(more.error());
                                                    }
                                                    if (more->has_value())
                                                    {
                                                        response.append_range(**more);
                                                    }
                                                    return {};
                                                });
            !read.has_value())
        {
            return read;
        }
        if (Status slept = clock.Sleep(100ms, cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    return {};
}

// SH7055-specific legacy unwrap, lines 704-717: check the complete,
// checksummed frame, remove the five-byte BEEF/opcode envelope and checksum,
// then inspect and strip the payload's length/failure prefix. MC68 has
// neither this payload prefix nor marker.
Result<std::uint32_t> UnwrapCrc(bytes::ByteView response)
{
    if (const std::optional<std::size_t> expected_size = DeclaredCrcFrameSize(response);
        expected_size.value_or(0) == 0 || response.size() != *expected_size || response.size() < 7 ||
        !DensoBeefKlineResponseOk(response, kDensoKlineOpCrc | 0x40U) ||
        response.back() != bytes::Sum8(response.first(response.size() - 1)))
    {
        return Fail(ErrorKind::kBadResponse, "Wrong or incomplete response from ECU during CRC check");
    }
    const bytes::ByteView payload = response.subspan(5, response.size() - 6);
    if (payload.empty())
    {
        return Fail(ErrorKind::kBadResponse, "Missing CRC response prefix");
    }
    if (payload.front() == 0x7F)
    {
        return Fail(ErrorKind::kBadResponse, "ECU marked CRC response failed");
    }
    if (payload.size() != 5)
    {
        return Fail(ErrorKind::kBadResponse, "Truncated CRC response from ECU");
    }
    return bytes::ReadU32Be(payload, 1);
}

// Legacy init_flash_write(), lines 752-873. SH7055's returned 32-bit
// lengths begin at offsets 6..9, one byte later than MC68.
Status InitFlashWrite(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                      IEventSink& events, bool test_write)
{
    for (const auto& [opcode, label] : {std::pair{kDensoKlineOpGetMaxMsgSize, "Max message length"},
                                        std::pair{kDensoKlineOpGetMaxBlockSize, "Flash block size"}})
    {
        Result<bytes::Bytes> response = DensoBeefKlineExchangeAck(
            transport, &clock, cancellation, DensoBeefKlineFrame(opcode), 200ms, 200ms, opcode, "flash init", 9);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        events.Log(LogLevel::kInfo, std::format("{}: 0x{:08X}", label, bytes::ReadU32Be(*response, 6)));
    }
    const std::uint8_t enable_opcode = test_write ? kDensoKlineOpFlashDisable : kDensoKlineOpFlashEnable;
    if (Result<bytes::Bytes> enabled =
            DensoBeefKlineExchangeAck(transport, &clock, cancellation, DensoBeefKlineFrame(enable_opcode), 200ms, 200ms,
                                      enable_opcode, "flash init");
        !enabled.has_value())
    {
        return std::unexpected(enabled.error());
    }
    return {};
}

} // namespace

Status SubaruDensoSh7055_02Executor::ConnectBootloader(IKlineFlashTransport& transport, IClock& clock,
                                                       const ICancellationToken& cancellation, IEventSink& events,
                                                       const SubaruDensoSh7055_02Plan& family_plan, bool read_ecu_id,
                                                       bool& kernel_alive, std::optional<std::string>& ecu_id) const
{
    Result<bytes::Bytes> probe = CheckCancelled(cancellation, "cancelled before connect")
                                     .and_then(
                                         [&]
                                         {
                                             events.Log(LogLevel::kInfo, "Checking if kernel is already running...");
                                             // Legacy
                                             // src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:
                                             // 108-131 and 1169-1198.
                                             return RequestKernelId(transport, clock, cancellation);
                                         });
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (DensoBeefKlineResponseOk(*probe, static_cast<bytes::Byte>(kDensoKlineOpId | 0x40U)))
    {
        kernel_alive = true;
        events.Log(LogLevel::kInfo, "Kernel already running");
        return {};
    }
    if (read_ecu_id)
    {
        Result<std::string> id = ReadEcuId(transport, cancellation, events, family_plan);
        if (!id.has_value())
        {
            return std::unexpected(id.error());
        }
        ecu_id = std::move(*id);
    }
    Result<bool> connected = ChangeBaud(transport, cancellation, 9600)
                                 .and_then([&] { return EnterBootMode(transport, clock, cancellation, events); })
                                 .and_then([&] { return InitBootloader(transport, cancellation); });
    if (!connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    if (*connected)
    {
        events.Log(LogLevel::kInfo, "Connected to bootloader");
    }
    if (Status slept = clock.Sleep(100ms, cancellation); !slept.has_value())
    {
        return slept;
    }
    if (!*connected)
    {
        if (Status drained = Drain(transport, cancellation, 100ms, "exhausted WRX drain"); !drained.has_value())
        {
            return drained;
        }
        return Fail(ErrorKind::kTimeout, "no response from bootloader");
    }
    if (Status drained = Drain(transport, cancellation, 10ms, "post-connect drain"); !drained.has_value())
    {
        return drained;
    }
    kernel_alive = false;
    return {};
}

Status SubaruDensoSh7055_02Executor::UploadKernel(IKlineFlashTransport& transport, IClock& clock,
                                                  const ICancellationToken& cancellation, IEventSink& events,
                                                  const KernelImage& kernel) const
{
    if (Status cancelled = CheckCancelled(cancellation, "cancelled before kernel upload"); !cancelled.has_value())
    {
        return cancelled;
    }
    if (kernel.bytes.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "kernel image is empty");
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
        byte = static_cast<bytes::Byte>((byte ^ 0x55U) + 0x10);
    }
    const std::uint32_t address = kernel.load_address;
    const std::uint32_t length = static_cast<std::uint32_t>(encrypted.size() + 4);
    // Not a full u24(address): the frame carries only the address's high two
    // bytes here (bits 23-8). The low byte is never emitted in this header --
    // bytes 6-9 below are a fixed transform of the literal 0x00, the checksum
    // placeholder overwritten at request[7], and the fixed 0x31/0x61 envelope
    // markers, none of which reference address.
    bytes::Bytes request =
        bytes::ComposeBeWithExtraCapacity(0, kDensoKlineOpUploadKernel, std::uint16_t(address >> 8U));
    bytes::AppendU24Be(request, length);
    request.push_back(static_cast<bytes::Byte>((0x00U ^ 0x55U) + 0x10));
    request.push_back(0x00);
    request.push_back(0x31);
    request.push_back(0x61);
    // Not composeBeWithChecksum: the first checksum is patched into the
    // middle of the frame at offset 7, and the second covers the frame plus
    // the encrypted payload appended afterwards.
    request[7] = fastecu::checksum::NegatedSum8(request);
    request.insert(request.end(), encrypted.begin(), encrypted.end());
    request.push_back(fastecu::checksum::NegatedSum8(request));

    events.Log(LogLevel::kInfo, "Sending kernel...");
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:297-321.
    // On Unix the OpenPort2/J2534 adapter requires the legacy 5000 ms quiet
    // period after the raw write and before reading its upload response.
    // IClock keeps that wait deterministic and cancellation-aware.
    const std::chrono::milliseconds post_upload_delay = transport.RequiresPostKernelUploadDelay() ? 5000ms : 0ms;
    return DensoBeefKlineExchangeOptional(transport, &clock, cancellation, request, post_upload_delay, 200ms)
        .and_then(
            [](const IKlineFlashTransport::OptionalBytes& upload_response) -> Status
            {
                if (upload_response.has_value() && !upload_response->empty())
                {
                    return Fail(ErrorKind::kBadResponse, "error on kernel upload");
                }
                return {};
            })
        .and_then(
            [&]
            {
                events.Log(LogLevel::kInfo, "Kernel uploaded successfully");
                return ChangeBaud(transport, cancellation, 62500);
            })
        .and_then([&] { return clock.Sleep(100ms, cancellation); })
        // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:323-352
        // and 1169-1198.
        .and_then([&] { return RequestKernelId(transport, clock, cancellation); })
        .and_then(
            [&](const bytes::Bytes& id) -> Status
            {
                if (id.size() <= 4)
                {
                    return Fail(ErrorKind::kBadResponse, "no valid response from ECU after kernel upload");
                }
                if (!DensoBeefKlineResponseOk(id, static_cast<bytes::Byte>(kDensoKlineOpId | 0x40U)))
                {
                    return Fail(ErrorKind::kBadResponse, "wrong response from ECU after kernel upload");
                }
                events.Log(LogLevel::kInfo, "Kernel is alive");
                return {};
            });
}

Result<bytes::Bytes> SubaruDensoSh7055_02Executor::ReadMem(IKlineFlashTransport& transport, IClock& clock,
                                                           const ICancellationToken& cancellation, IEventSink& events,
                                                           const MemoryRegion& region) const
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:360-455:
    // request consecutive 0x400-byte pages with SUB_KERNEL_READ_AREA, remove
    // the five-byte response envelope and its trailing checksum, then retain
    // the received bytes. SH7055_02 has no rblocks skip branch in this path.
    bytes::Bytes mapdata;
    std::uint32_t address = region.start;
    std::uint32_t remaining_pages = (region.length + kReadPageSize - 1) / kReadPageSize;
    events.Progress(0, static_cast<int>(region.length));

    while (remaining_pages > 0)
    {
        if (Status cancelled = CheckCancelled(cancellation, "cancelled during read"); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        const auto payload = ComposeBe(0x00_b, U24{address}, std::uint16_t(kReadPageSize));
        // Legacy lines 415-421 settle for 10ms and use serial_read_extra_long_timeout (3000ms).
        Result<IKlineFlashTransport::OptionalBytes> response = DensoBeefKlineExchangeOptional(
            transport, &clock, cancellation, DensoBeefKlineFrame(kDensoKlineOpReadArea, payload), 10ms, 3000ms);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        // Legacy lines 423-437 only accept the BEEF / 0x43 response envelope
        // before removing its fixed header and final checksum. Its fixed page
        // request (lines 415-416) requires a full 0x400-byte payload; reject
        // short replies rather than returning a silently truncated ROM.
        if (!response->has_value() ||
            !DensoBeefKlineResponseOk(**response, static_cast<bytes::Byte>(kDensoKlineOpReadArea | 0x40U)) ||
            (**response).size() != kReadPageSize + 6)
        {
            return Fail(ErrorKind::kBadResponse, "Wrong response from ECU during read");
        }
        const bytes::Bytes& received = **response;
        mapdata.insert(mapdata.end(), received.begin() + 5, received.end() - 1);
        address += kReadPageSize;
        --remaining_pages;
        events.Progress(static_cast<int>(mapdata.size()), static_cast<int>(region.length));
        // Legacy line 466 paces successive requests by 1ms.
        if (Status slept = clock.Sleep(1ms, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    if (mapdata.size() > region.length)
    {
        mapdata.resize(region.length);
    }
    events.Progress(static_cast<int>(region.length), static_cast<int>(region.length));
    return mapdata;
}

Result<std::uint32_t> SubaruDensoSh7055_02Executor::ReadBlockCrc(IKlineFlashTransport& transport, IClock& clock,
                                                                 const ICancellationToken& cancellation,
                                                                 const MemoryRegion& block) const
{
    // Legacy check_romcrc(), lines 645-749: request the ECU CRC for the
    // physical [start, start + length) block.
    const Result<std::uint32_t> crc = DensoBeefKlineRequestBlockCrc(transport, cancellation, block)
                                          .and_then(
                                              [&](bytes::Bytes response)
                                              {
                                                  return ReadCrcContinuation(transport, clock, cancellation, response)
                                                      .and_then([&] { return UnwrapCrc(response); });
                                              });
    if (!crc.has_value())
    {
        return crc;
    }
    // Legacy lines 742 and 748 perform a short read after either compare
    // outcome, so drain every successful CRC decode before returning it.
    return Drain(transport, cancellation, 200ms, "CRC response drain").transform([&] { return *crc; });
}

Status SubaruDensoSh7055_02Executor::FlashBlock(IKlineFlashTransport& transport, IClock& clock,
                                                const ICancellationToken& cancellation, IEventSink& events,
                                                bytes::ByteView image, const MemoryRegion& block, bool test_write) const
{
    if (block.start > image.size() || block.length > image.size() - block.start ||
        block.length % kWriteChunkSize != 0 || block.length % kCommitBlockSize != 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "flash block is not represented by the image");
    }
    if (!test_write)
    {
        // Legacy flash_block(), lines 982-1019: erase, settle 500ms, then
        // wait up to 3000ms for the 0x65 acknowledgment.
        events.Log(LogLevel::kInfo, "Erasing flash page...");
        if (Result<bytes::Bytes> erased = DensoBeefKlineExchangeAck(
                transport, &clock, cancellation, DensoBeefKlineFrame(kDensoKlineOpBlankPage, ComposeBe(block.start)),
                500ms, 3000ms, kDensoKlineOpBlankPage, "erase");
            !erased.has_value())
        {
            return std::unexpected(erased.error());
        }
        events.Log(LogLevel::kInfo, "Erased");
    }
    const std::uint8_t commit_opcode = test_write ? kDensoKlineOpValidateFlashBuffer : kDensoKlineOpCommitFlashBuffer;
    for (std::uint32_t offset = 0; offset < block.length;)
    {
        if (Status cancelled = CheckCancelled(cancellation, "cancelled during block write"); !cancelled.has_value())
        {
            return cancelled;
        }
        const std::uint32_t chunk_address = block.start + offset;
        const bytes::Bytes payload =
            DensoBeefKlineWritePayload(chunk_address, image.subspan(chunk_address, kWriteChunkSize));
        // SH7055 legacy lines 1048-1050 actively settle 50ms and use the
        // normal 2000ms timeout; MC68's corresponding delay is commented.
        if (Result<bytes::Bytes> written = DensoBeefKlineExchangeAck(
                transport, &clock, cancellation, DensoBeefKlineFrame(kDensoKlineOpWriteFlashBuffer, payload), 50ms,
                2000ms, kDensoKlineOpWriteFlashBuffer, "write");
            !written.has_value())
        {
            return std::unexpected(written.error());
        }
        offset += kWriteChunkSize;
        if (offset % kCommitBlockSize == 0)
        {
            const std::uint32_t commit_start = block.start + offset - kCommitBlockSize;
            const auto commit_payload =
                ComposeBe(commit_start, std::uint16_t(kCommitBlockSize),
                          fastecu::checksum::Crc32(image.subspan(commit_start, kCommitBlockSize)));
            if (Result<bytes::Bytes> committed = DensoBeefKlineExchangeAck(
                    transport, &clock, cancellation, DensoBeefKlineFrame(commit_opcode, commit_payload), 200ms, 3000ms,
                    commit_opcode, "commit");
                !committed.has_value())
            {
                return std::unexpected(committed.error());
            }
        }
        events.Progress(static_cast<int>(offset), static_cast<int>(block.length));
    }
    // Legacy line 1159 discards one final short-timeout response.
    return Drain(transport, cancellation, 200ms, "flash block drain");
}

Status SubaruDensoSh7055_02Executor::WriteMem(IKlineFlashTransport& transport, IClock& clock,
                                              const ICancellationToken& cancellation, IEventSink& events,
                                              bytes::ByteView image, const std::string& mcu_name, bool test_write) const
{
    const FlashDevice *device = FindFlashDevice(mcu_name);
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "Unknown MCU type");
    }
    const std::vector<MemoryRegion> blocks = detail::MakeEraseRegions(*device);
    const auto physical_end = [](const MemoryRegion& region)
    { return static_cast<std::uint64_t>(region.start) + region.length; };
    if (image.size() != device->romsize ||
        std::ranges::any_of(blocks, [&](const MemoryRegion& region) { return physical_end(region) > image.size(); }))
    {
        return Fail(ErrorKind::kInvalidConfig, "ROM image does not match the flash device");
    }

    // Legacy write_mem()/get_changed_blocks(), lines 484-637, with
    // reflash_block()'s programming-voltage query at lines 914-944.
    return DensoBeefKlineWriteChangedBlocks(
        transport, cancellation, events, blocks, image, test_write,
        [&](const MemoryRegion& block) { return ReadBlockCrc(transport, clock, cancellation, block); }, [&]
        { return InitFlashWrite(transport, clock, cancellation, events, test_write); }, [&](const MemoryRegion& block)
        { return FlashBlock(transport, clock, cancellation, events, image, block, test_write); });
}

Result<KlineConfig> SubaruDensoSh7055_02Executor::Setup(const SubaruDensoSh7055_02Plan& family_plan) const
{
    return KlineConfig{
        .baud = 62500, .iso14230 = false, .tester_id = family_plan.tester_id, .target_id = family_plan.target_id};
}

Status SubaruDensoSh7055_02Executor::BeforeTransportOpen(const ICancellationToken& cancellation) const
{
    return CheckCancelled(cancellation, "cancelled after transport configuration")
        .and_then([&] { return CheckCancelled(cancellation, "cancelled before opening transport"); });
}

Result<FlashExecutionResult> SubaruDensoSh7055_02Executor::Run(const SubaruDensoSh7055_02Plan& family_plan,
                                                               const FlashPlan& plan, IKlineFlashTransport& kline,
                                                               IClock& clock, const ICancellationToken& cancellation,
                                                               IEventSink& events)
{
    bool kernel_alive = false;
    std::optional<std::string> ecu_id;
    // Plan admission is the cancellation checkpoint before the LEC lines.
    if (Status connected =
            kline.DisableLecLines()
                .and_then([&] { return CheckCancelled(cancellation, "cancelled after disabling LEC lines"); })
                .and_then([&] { return Drain(kline, cancellation, 10ms, "initial drain"); })
                .and_then(
                    [&]
                    {
                        return ConnectBootloader(kline, clock, cancellation, events, family_plan,
                                                 family_plan.read_ecu_id, kernel_alive, ecu_id);
                    });
        !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    if (!kernel_alive)
    {
        if (!plan.Kernel().has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "SH7055_02 requires a kernel image");
        }
        if (Status uploaded = UploadKernel(kline, clock, cancellation, events, *plan.Kernel()); !uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
    }

    if (plan.Operation() == FlashOperation::kRead)
    {
        Result<bytes::Bytes> read = ReadMem(kline, clock, cancellation, events, plan.TransferRegion());
        if (!read.has_value())
        {
            return std::unexpected(read.error());
        }
        return FlashExecutionResult{.operation = plan.Operation(), .read_bytes = std::move(*read), .rom_id = ecu_id};
    }

    if (!plan.Image().has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "SH7055_02 write requires a ROM image");
    }
    if (Status written = WriteMem(kline, clock, cancellation, events, *plan.Image(), plan.McuName(),
                                  plan.Operation() == FlashOperation::kTestWrite);
        !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{.operation = plan.Operation(), .read_bytes = std::nullopt};
}

} // namespace fastecu::flash
