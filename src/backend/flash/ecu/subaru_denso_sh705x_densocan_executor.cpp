#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor.h"

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/transfer_progress.h"

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;

constexpr std::uint32_t kIsoRequestId = 0x7E0;
constexpr std::uint32_t kRawTransmitId = 0x000FFFFE;
constexpr std::uint32_t kRawReceiveId = 0x21;
constexpr std::uint16_t kStartComm = 0xBEEF;
constexpr std::uint32_t kReadPageSize = 0x400;
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitBlockSize = 0x1000;
constexpr std::chrono::milliseconds kKernelIdDelay{200};
constexpr std::chrono::milliseconds kCrcComparisonDelay{5};
constexpr std::chrono::milliseconds kKernelIdTimeout{800};
constexpr std::chrono::milliseconds kRawTimeout{800};
constexpr std::chrono::milliseconds kPageTimeout{3000};
constexpr std::chrono::milliseconds kCrcTimeout{3000};
constexpr std::chrono::milliseconds kFlashInitTimeout{500};
constexpr std::chrono::milliseconds kFlashBufferAckTimeout{800};
constexpr std::chrono::milliseconds kFlashTimeout{3000};
constexpr std::string_view kReflashRecoveryWarning =
    "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most likely still running and "
    "receiving commands!";

Status CheckCancelled(const ICancellationToken& cancellation, std::string detail)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::move(detail));
    }
    return {};
}

std::uint64_t ElapsedMilliseconds(std::chrono::steady_clock::time_point start,
                                  std::chrono::steady_clock::time_point end)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 1U;
}

bytes::Bytes IsoRequest(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes request = ComposeBe(kIsoRequestId, kStartComm, static_cast<std::uint16_t>(payload.size() + 1),
                                     static_cast<bytes::Byte>(opcode));
    request.insert(request.end(), payload.begin(), payload.end());
    return request;
}

// Legacy request_kernel_id(),
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.cpp:1435-1484.
// The message declares a one-byte BEEF payload but is passed to the legacy
// ISO-15765 driver as a fixed twelve-byte CAN wire buffer with three zeros.
bytes::Bytes KernelIdRequest()
{
    return {0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
}

Status IsoWrite(IMixedCanFlashTransport& transport, bytes::ByteView request, const ICancellationToken& cancellation,
                std::string_view subject)
{
    if (Status cancelled = CheckCancelled(cancellation, std::format("cancelled before {} write", subject)); !cancelled)
    {
        return cancelled;
    }
    if (Status written = transport.WriteIso15765(request, cancellation); !written)
    {
        return written;
    }
    return CheckCancelled(cancellation, std::format("cancelled after {} write", subject));
}

Result<std::optional<bytes::Bytes>> IsoRead(IMixedCanFlashTransport& transport, std::chrono::milliseconds timeout,
                                            const ICancellationToken& cancellation, std::string_view subject)
{
    if (Status cancelled = CheckCancelled(cancellation, std::format("cancelled before {} read", subject)); !cancelled)
    {
        return std::unexpected(cancelled.error());
    }
    Result<std::optional<bytes::Bytes>> received = transport.ReadIso15765(timeout, cancellation);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (Status cancelled = CheckCancelled(cancellation, std::format("cancelled after {} read", subject)); !cancelled)
    {
        return std::unexpected(cancelled.error());
    }
    return received;
}

Status RawWrite(IMixedCanFlashTransport& transport, cdbg::CanFrame frame, const ICancellationToken& cancellation,
                std::string_view subject)
{
    if (Status cancelled = CheckCancelled(cancellation, std::format("cancelled before {} write", subject)); !cancelled)
    {
        return cancelled;
    }
    if (Status written = transport.WriteRaw(frame, cancellation); !written)
    {
        return written;
    }
    return CheckCancelled(cancellation, std::format("cancelled after {} write", subject));
}

Result<std::optional<cdbg::CanFrame>> RawRead(IMixedCanFlashTransport& transport, std::chrono::milliseconds timeout,
                                              const ICancellationToken& cancellation, std::string_view subject)
{
    if (Status cancelled = CheckCancelled(cancellation, std::format("cancelled before {} read", subject)); !cancelled)
    {
        return std::unexpected(cancelled.error());
    }
    Result<std::optional<cdbg::CanFrame>> received = transport.ReadRaw(timeout, cancellation);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (Status cancelled = CheckCancelled(cancellation, std::format("cancelled after {} read", subject)); !cancelled)
    {
        return std::unexpected(cancelled.error());
    }
    return received;
}

Status ExpectIsoResponse(bytes::ByteView response, std::uint8_t expected_opcode, std::size_t min_payload,
                         std::string_view subject)
{
    // DensoCAN BEEF is a proprietary kernel envelope, not a UDS PDU. Its
    // fixed transport identifier occupies 0..3; BEEF, declared length and
    // opcode then occupy 4..8. Legacy checks the BEEF/opcode bytes at
    // 512-657, 820-905 and 906-1395; the guards below also make each later
    // indexed byte safe.
    if (response.size() < 9)
    {
        return Fail(ErrorKind::kBadResponse, std::format("{} response is shorter than the BEEF envelope", subject));
    }
    if (response[0] != 0x00 || response[1] != 0x00 || response[2] != 0x07 || response[3] != 0xE8 ||
        response[4] != 0xBE || response[5] != 0xEF || response[8] != expected_opcode)
    {
        return Fail(ErrorKind::kBadResponse, std::format("Wrong response from ECU during {}", subject));
    }
    const std::size_t declared_payload = bytes::ReadU16Be(response, 6);
    if (declared_payload == 0 || declared_payload > response.size() - 8 || declared_payload - 1 < min_payload ||
        response.size() - 9 < min_payload)
    {
        return Fail(ErrorKind::kBadResponse, std::format("Truncated response from ECU during {}", subject));
    }
    return {};
}

Status ExpectRawResponse(const cdbg::CanFrame& response, std::uint8_t first, std::uint8_t second,
                         std::string_view subject)
{
    if (response.id != kRawReceiveId)
    {
        return Fail(ErrorKind::kBadResponse, std::format("Wrong raw CAN response ID during {}", subject));
    }
    if (response.payload.size() < 2)
    {
        return Fail(ErrorKind::kBadResponse, std::format("Truncated raw CAN response during {}", subject));
    }
    if (response.payload[0] != first || response.payload[1] != second)
    {
        return Fail(ErrorKind::kBadResponse, std::format("Wrong raw CAN response during {}", subject));
    }
    return {};
}

Result<bool> ProbeKernel(IMixedCanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                         IEventSink& events)
{
    // Legacy connect_bootloader(), revision 59f4e442 lines 106-153. Any
    // absent, short, malformed, wrong-id, or wrong-content initial reply is
    // treated as "kernel absent" and falls through to the raw bootloader.
    // Typed cancellation and adapter loss remain terminal.
    events.Log(LogLevel::kInfo, "Checking if Kernel already running...");
    events.Log(LogLevel::kInfo, "Requesting kernel ID");
    const bytes::Bytes request = KernelIdRequest();
    if (Status written = IsoWrite(transport, request, cancellation, "kernel ID"); !written)
    {
        return std::unexpected(written.error());
    }
    if (Status waited = clock.Sleep(kKernelIdDelay, cancellation); !waited)
    {
        return std::unexpected(waited.error());
    }
    Result<std::optional<bytes::Bytes>> received = IsoRead(transport, kKernelIdTimeout, cancellation, "kernel ID");
    if (!received)
    {
        if (received.error().kind == ErrorKind::kCancelled || received.error().kind == ErrorKind::kDisconnected)
        {
            return std::unexpected(received.error());
        }
        events.Log(LogLevel::kError, "No valid response from ECU");
        events.Log(LogLevel::kInfo, "No response from kernel, continue initializing bootloader...");
        return false;
    }
    if (!received->has_value())
    {
        events.Log(LogLevel::kError, "No valid response from ECU");
        events.Log(LogLevel::kInfo, "No response from kernel, continue initializing bootloader...");
        return false;
    }
    if (Status valid = ExpectIsoResponse(**received, 0x41, 0, "kernel ID"); !valid)
    {
        events.Log(LogLevel::kError, "Wrong response from ECU while requesting kernel ID");
        events.Log(LogLevel::kInfo, "No response from kernel, continue initializing bootloader...");
        return false;
    }
    std::string kernel_id;
    kernel_id.reserve((**received).size() - 9);
    for (const bytes::Byte byte : bytes::ByteView(**received).subspan(9))
    {
        kernel_id.push_back(static_cast<char>(byte));
    }
    events.Log(LogLevel::kInfo, std::format("Kernel ID: {}", kernel_id));
    return true;
}

Status RequireKernelId(IMixedCanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                       IEventSink& events)
{
    // Legacy upload_kernel(), lines 475-507, repeats the same proprietary
    // BEEF exchange only after the raw bootloader jump and ISO transition.
    events.Log(LogLevel::kInfo, "Requesting kernel ID");
    const bytes::Bytes request = KernelIdRequest();
    if (Status written = IsoWrite(transport, request, cancellation, "kernel ID"); !written)
    {
        return written;
    }
    if (Status waited = clock.Sleep(kKernelIdDelay, cancellation); !waited)
    {
        return waited;
    }
    Result<std::optional<bytes::Bytes>> received = IsoRead(transport, kKernelIdTimeout, cancellation, "kernel ID");
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no response from ECU after kernel upload");
    }
    if (Status valid = ExpectIsoResponse(**received, 0x41, 0, "kernel ID"); !valid)
    {
        return valid;
    }
    std::string kernel_id;
    kernel_id.reserve((**received).size() - 9);
    for (const bytes::Byte byte : bytes::ByteView(**received).subspan(9))
    {
        kernel_id.push_back(static_cast<char>(byte));
    }
    events.Log(LogLevel::kInfo, std::format("Kernel ID: {}", kernel_id));
    return {};
}

Status WakeBootloader(IMixedCanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                      IEventSink& events)
{
    // Legacy connect_bootloader(), lines 157-222. The portable executor
    // retains the exact 1,000 raw wake frames, adding a cooperative check at
    // every iteration so this bounded loop cannot become an uncancellable UI
    // stall.
    const cdbg::CanFrame wake{.id = kRawTransmitId, .payload = {0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    events.Log(LogLevel::kInfo, "Initializing bootloader");
    for (int count = 0; count < 1000; ++count)
    {
        if (Status cancelled = CheckCancelled(cancellation, "cancelled during DensoCAN wake"); !cancelled)
        {
            return cancelled;
        }
        if (Status written = RawWrite(transport, wake, cancellation, "DensoCAN wake"); !written)
        {
            return written;
        }
        if (Status slept = clock.Sleep(3ms, cancellation); !slept)
        {
            return slept;
        }
        events.Progress(count + 1, 1000);
    }
    if (Status cleared = transport.ClearReceiveBuffer(); !cleared)
    {
        return cleared;
    }

    events.Log(LogLevel::kInfo, "Check if connected to bootloader");
    const cdbg::CanFrame check{.id = kRawTransmitId, .payload = {0x7A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    if (Status written = RawWrite(transport, check, cancellation, "bootloader handshake"); !written)
    {
        return written;
    }
    Result<std::optional<cdbg::CanFrame>> received =
        RawRead(transport, kRawTimeout, cancellation, "bootloader handshake");
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no response from DensoCAN bootloader");
    }
    if (Status valid = ExpectRawResponse(**received, 0x7A, 0x96, "bootloader handshake"); !valid)
    {
        return valid;
    }
    events.Log(LogLevel::kDebug, "Connected to bootloader");
    return {};
}

cdbg::CanFrame RawAddressCommand(std::uint8_t command, std::uint32_t address)
{
    return {.id = kRawTransmitId,
            .payload = {0x7A, command, static_cast<bytes::Byte>(address >> 24U),
                        static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                        static_cast<bytes::Byte>(address), 0x00, 0x00}};
}

Status SetRawKernelAddress(IMixedCanFlashTransport& transport, std::uint32_t address,
                           const ICancellationToken& cancellation, std::string_view subject)
{
    if (Status written = RawWrite(transport, RawAddressCommand(0x9C, address), cancellation, subject); !written)
    {
        return written;
    }
    Result<std::optional<cdbg::CanFrame>> received = RawRead(transport, kRawTimeout, cancellation, subject);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, std::format("no response from ECU during {}", subject));
    }
    return ExpectRawResponse(**received, 0x7A, 0x9C, subject);
}

Status UploadKernel(IMixedCanFlashTransport& transport, const KernelImage& kernel, IClock& clock,
                    const ICancellationToken& cancellation, IEventSink& events)
{
    // Legacy upload_kernel(), lines 227-507. This raw Denso bootloader path
    // uses a 29-bit transmit ID, six payload bytes per 0x7A/0xAE block, and
    // a zero-padded final block. BEEF is not used in this mode.
    if (kernel.bytes.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN kernel image is empty");
    }
    if (Status cancelled = CheckCancelled(cancellation, "cancelled before kernel upload"); !cancelled)
    {
        return cancelled;
    }
    bytes::Bytes padded = kernel.bytes;
    padded.resize((padded.size() + 5U) / 6U * 6U, 0);
    if (padded.empty() || padded.size() > std::numeric_limits<std::uint32_t>::max() - kernel.load_address - 1U)
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN padded kernel range overflows");
    }

    events.Log(LogLevel::kDebug, std::format("Start address to upload kernel: {:x}", kernel.load_address));
    events.Log(LogLevel::kInfo, "Set kernel upload address");
    if (Status addressed = SetRawKernelAddress(transport, kernel.load_address, cancellation, "kernel upload address");
        !addressed)
    {
        return addressed;
    }
    events.Log(LogLevel::kDebug, "Kernel load address set");

    events.Log(LogLevel::kInfo, "Uploading kernel, please wait...");
    events.Log(LogLevel::kDebug, std::format("Sending {} blocks", padded.size() / 6));
    for (std::size_t offset = 0; offset < padded.size(); offset += 6)
    {
        if (Status cancelled = CheckCancelled(cancellation, "cancelled during kernel upload"); !cancelled)
        {
            return cancelled;
        }
        cdbg::CanFrame block{.id = kRawTransmitId, .payload = {0x7A, 0xAE}};
        block.payload.insert(block.payload.end(), padded.begin() + static_cast<std::ptrdiff_t>(offset),
                             padded.begin() + static_cast<std::ptrdiff_t>(offset + 6));
        if (Status written = RawWrite(transport, std::move(block), cancellation, "kernel upload block"); !written)
        {
            return written;
        }
        if (Status slept = clock.Sleep(1ms, cancellation); !slept)
        {
            return slept;
        }
        events.Progress(static_cast<int>(offset + 6), static_cast<int>(padded.size()));
    }

    std::uint32_t folded_checksum = 0;
    for (const bytes::Byte byte : padded)
    {
        folded_checksum += byte;
        folded_checksum = ((folded_checksum >> 8U) & 0xFFU) + (folded_checksum & 0xFFU);
    }
    events.Log(LogLevel::kDebug, std::format("All kernel blocks sent, checksum: 0x{:x}", folded_checksum));

    const std::uint32_t end_plus_one = kernel.load_address + static_cast<std::uint32_t>(padded.size()) + 1U;
    if (Status written = RawWrite(transport, RawAddressCommand(0xB4, end_plus_one), cancellation, "kernel checksum");
        !written)
    {
        return written;
    }
    events.Log(LogLevel::kDebug, "Verifying kernel checksum, please wait...");
    if (Status slept = clock.Sleep(200ms, cancellation); !slept)
    {
        return slept;
    }
    Result<std::optional<cdbg::CanFrame>> checksum = RawRead(transport, kRawTimeout, cancellation, "kernel checksum");
    if (!checksum)
    {
        return std::unexpected(checksum.error());
    }
    if (!checksum->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no response from ECU during kernel checksum");
    }
    if (Status valid = ExpectRawResponse(**checksum, 0x7A, 0xB1, "kernel checksum"); !valid)
    {
        return valid;
    }
    events.Log(LogLevel::kDebug, "Checksum ok");

    if (Status addressed = SetRawKernelAddress(transport, kernel.load_address, cancellation, "kernel jump address");
        !addressed)
    {
        return addressed;
    }
    events.Log(LogLevel::kInfo, "Kernel uploaded, jump to kernel");
    const cdbg::CanFrame jump{.id = kRawTransmitId, .payload = {0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    if (Status written = RawWrite(transport, jump, cancellation, "kernel jump"); !written)
    {
        return written;
    }
    if (Status slept = clock.Sleep(200ms, cancellation); !slept)
    {
        return slept;
    }
    Result<std::optional<cdbg::CanFrame>> jumped = RawRead(transport, 50ms, cancellation, "kernel jump");
    if (!jumped)
    {
        return std::unexpected(jumped.error());
    }
    // Legacy lines 446-463 log but deliberately do not return on a missing
    // or wrong 7A/A0 response. Preserve both the tolerant outcome and its
    // operator-visible error records.
    if (jumped->has_value())
    {
        if (Status jump_response = ExpectRawResponse(**jumped, 0x7A, 0xA0, "kernel jump"); !jump_response)
        {
            events.Log(LogLevel::kError, "Wrong response from ECU");
        }
    }
    else
    {
        events.Log(LogLevel::kError, "No valid response from ECU");
    }
    return {};
}

Result<bytes::Bytes> ReadMem(IMixedCanFlashTransport& transport, const MemoryRegion& region, IClock& clock,
                             const ICancellationToken& cancellation, IEventSink& events, PhaseReporter& phase)
{
    // Legacy read_mem(), lines 512-657, appends each BEEF page exactly as it
    // arrived. Every 0x400-byte response must contain its entire raw page
    // before it is indexed.
    bytes::Bytes rom;
    rom.reserve(region.length);
    events.Log(LogLevel::kInfo, "Start reading ROM, please wait...");
    for (std::uint32_t offset = 0; offset < region.length; offset += kReadPageSize)
    {
        const auto loop_started = clock.Now();
        if (Status cancelled = CheckCancelled(cancellation, "cancelled during ROM read"); !cancelled)
        {
            return std::unexpected(cancelled.error());
        }
        const std::uint32_t address = region.start + offset;
        const bytes::Bytes request = IsoRequest(0x03, ComposeBe(0x00_b, U24(address), std::uint16_t{kReadPageSize}));
        if (Status written = IsoWrite(transport, request, cancellation, "ROM read"); !written)
        {
            return std::unexpected(written.error());
        }
        Result<std::optional<bytes::Bytes>> received = IsoRead(transport, kPageTimeout, cancellation, "ROM read");
        if (!received)
        {
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            return Fail(ErrorKind::kTimeout, "no response from ECU during ROM read");
        }
        if (Status valid = ExpectIsoResponse(**received, 0x43, kReadPageSize, "ROM read"); !valid)
        {
            return std::unexpected(valid.error());
        }
        const bytes::ByteView raw_page(**received);
        const std::uint64_t elapsed_ms = ElapsedMilliseconds(loop_started, clock.Now());
        const TransferRate rate = ComputeTransferRate(kReadPageSize, elapsed_ms, region.length - offset);
        events.Log(LogLevel::kInfo, FormatReadProgress(address, kReadPageSize, rate));
        rom.insert(rom.end(), raw_page.begin() + 9, raw_page.begin() + 9 + kReadPageSize);
        const int done = static_cast<int>(offset + kReadPageSize);
        events.Progress(done, static_cast<int>(region.length));
        phase.Update(done);
    }
    events.Log(LogLevel::kInfo, "ROM read ready");
    phase.Complete();
    return rom;
}

Result<std::uint32_t> ReadBlockCrc(IMixedCanFlashTransport& transport, const MemoryRegion& block,
                                   const ICancellationToken& cancellation)
{
    // Legacy check_romcrc(), lines 820-905. The seven-byte CRC request body
    // is [address:4][00][length:3]; response bytes 9..12 hold the ECU CRC.
    const bytes::Bytes request = IsoRequest(0x02, ComposeBe(block.start, 0x00_b, U24(block.length)));
    if (Status written = IsoWrite(transport, request, cancellation, "ROM CRC"); !written)
    {
        return std::unexpected(written.error());
    }
    Result<std::optional<bytes::Bytes>> received = IsoRead(transport, kCrcTimeout, cancellation, "ROM CRC");
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no response from ECU during ROM CRC");
    }
    if (Status valid = ExpectIsoResponse(**received, 0x42, 4, "ROM CRC"); !valid)
    {
        return std::unexpected(valid.error());
    }
    const std::uint32_t crc = bytes::ReadU32Be(**received, 9);
    // Legacy lines 883 and 904 intentionally perform this short stale-byte
    // drain after both equal and unequal CRC outcomes.
    Result<std::optional<bytes::Bytes>> drained = IsoRead(transport, 200ms, cancellation, "ROM CRC drain");
    if (!drained)
    {
        return std::unexpected(drained.error());
    }
    return crc;
}

Status SetFlashMode(IMixedCanFlashTransport& transport, bool enabled, const ICancellationToken& cancellation)
{
    const std::uint8_t command = enabled ? 0x20 : 0x21;
    if (Status written = IsoWrite(transport, IsoRequest(command), cancellation, "flash mode"); !written)
    {
        return written;
    }
    Result<std::optional<bytes::Bytes>> received = IsoRead(transport, kFlashInitTimeout, cancellation, "flash mode");
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no response from ECU during flash mode setup");
    }
    return ExpectIsoResponse(**received, static_cast<std::uint8_t>(command | 0x40U), 0, "flash mode");
}

Status InitFlashWrite(IMixedCanFlashTransport& transport, bool test_write, const ICancellationToken& cancellation,
                      IEventSink& events)
{
    // Legacy init_flash_write(), lines 906-1059. Returned max lengths are
    // retained as diagnostic evidence; the legacy flash_block() explicitly
    // selects 0x1000 commit windows regardless of the reported block size.
    for (const std::uint8_t command : {std::uint8_t{0x05}, std::uint8_t{0x06}})
    {
        events.Log(LogLevel::kInfo, command == 0x05 ? "Check max message length" : "Check flashblock size");
        if (Status written = IsoWrite(transport, IsoRequest(command), cancellation, "flash initialization"); !written)
        {
            return written;
        }
        Result<std::optional<bytes::Bytes>> received =
            IsoRead(transport, kFlashInitTimeout, cancellation, "flash initialization");
        if (!received)
        {
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            return Fail(ErrorKind::kTimeout, "no response from ECU during flash initialization");
        }
        if (Status valid =
                ExpectIsoResponse(**received, static_cast<std::uint8_t>(command | 0x40U), 4, "flash initialization");
            !valid)
        {
            return valid;
        }
        const std::uint32_t value = bytes::ReadU32Be(**received, 9);
        events.Log(LogLevel::kInfo, std::format(": 0x{:04x}", value));
    }
    // Legacy test write sends FLASH_DISABLE instead of FLASH_ENABLE at
    // lines 1014-1019, then validates rather than commits each 0x1000 block.
    events.Log(LogLevel::kInfo, test_write ? "Test write mode on, no actual flash write is performed"
                                           : "Test write mode off, perform actual flash write");
    if (Status mode = SetFlashMode(transport, !test_write, cancellation); !mode)
    {
        return mode;
    }
    events.Log(LogLevel::kError, "Flash mode succesfully set");
    return {};
}

Status QueryProgrammingVoltage(IMixedCanFlashTransport& transport, const ICancellationToken& cancellation,
                               IEventSink& events)
{
    // Legacy reflash_block(), lines 1087-1150. Require bytes 9 and 10 before
    // decoding the voltage; legacy's >7 check did not make that access safe.
    events.Log(LogLevel::kInfo, "Check flash voltage");
    if (Status written = IsoWrite(transport, IsoRequest(0x04), cancellation, "programming voltage"); !written)
    {
        return written;
    }
    Result<std::optional<bytes::Bytes>> received =
        IsoRead(transport, kFlashInitTimeout, cancellation, "programming voltage");
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no response from ECU during programming-voltage query");
    }
    if (Status valid = ExpectIsoResponse(**received, 0x44, 2, "programming voltage"); !valid)
    {
        return valid;
    }
    const std::uint16_t voltage_raw = bytes::ReadU16Be(**received, 9);
    const float voltage = static_cast<float>(voltage_raw) / 50.0F;
    events.Log(LogLevel::kInfo, std::format(": {:g}V", voltage));
    return {};
}

Status FlashBlock(IMixedCanFlashTransport& transport, bytes::ByteView image, const MemoryRegion& block, bool test_write,
                  IClock& clock, const ICancellationToken& cancellation, IEventSink& events, PhaseReporter& phase,
                  bool& destructive_erase_succeeded, std::uint32_t& flashbytesindex, std::uint32_t flashbytescount)
{
    // Legacy flash_block(), lines 1153-1395. Each physical block is blanked
    // only for a real write, transferred in 0x200 chunks, and committed (or
    // validated for test-write) in fixed 0x1000 windows.
    if (block.start > image.size() || block.length > image.size() - block.start ||
        block.length % kWriteChunkSize != 0 || block.length % kCommitBlockSize != 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "flash block is not represented by the raw ROM image");
    }
    if (!test_write)
    {
        events.Log(LogLevel::kInfo,
                   std::format("Flash page erase addr: 0x{:08x} len: 0x{:08x}", block.start, block.length));
        events.Log(LogLevel::kInfo, "Erasing flash page...");
        if (Status written = IsoWrite(transport, IsoRequest(0x25, ComposeBe(block.start)), cancellation, "flash erase");
            !written)
        {
            return written;
        }
        Result<std::optional<bytes::Bytes>> erased = IsoRead(transport, kFlashTimeout, cancellation, "flash erase");
        if (!erased)
        {
            return std::unexpected(erased.error());
        }
        if (!erased->has_value())
        {
            return Fail(ErrorKind::kTimeout, "no response from ECU during flash erase");
        }
        if (Status valid = ExpectIsoResponse(**erased, 0x65, 0, "flash erase"); !valid)
        {
            return valid;
        }
        destructive_erase_succeeded = true;
        events.Log(LogLevel::kInfo, " erased");
    }

    events.Log(LogLevel::kInfo,
               std::format("Start flash write addr: 0x{:08x} len: 0x{:08x}", block.start, block.length));

    for (std::uint32_t offset = 0; offset < block.length; offset += kWriteChunkSize)
    {
        const auto loop_started = clock.Now();
        if (Status cancelled = CheckCancelled(cancellation, "cancelled during flash buffer transfer"); !cancelled)
        {
            return cancelled;
        }
        const std::uint32_t address = block.start + offset;
        const bytes::Bytes request = IsoRequest(0x22, ComposeBe(address, image.subspan(address, kWriteChunkSize)));
        if (Status written = IsoWrite(transport, request, cancellation, "flash buffer transfer"); !written)
        {
            return written;
        }
        Result<std::optional<bytes::Bytes>> received =
            IsoRead(transport, kFlashBufferAckTimeout, cancellation, "flash buffer transfer");
        if (!received)
        {
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            return Fail(ErrorKind::kTimeout, "no response from ECU during flash buffer transfer");
        }
        if (Status valid = ExpectIsoResponse(**received, 0x62, 0, "flash buffer transfer"); !valid)
        {
            return valid;
        }
        const std::uint64_t elapsed_ms = ElapsedMilliseconds(loop_started, clock.Now());
        const std::uint32_t bytes_after = flashbytesindex + kWriteChunkSize;
        const TransferRate rate = ComputeTransferRate(kWriteChunkSize, elapsed_ms, flashbytescount - bytes_after);
        events.Log(LogLevel::kDebug, "Data written to flash buffer");
        events.Log(LogLevel::kInfo, FormatWriteProgress(address, (100U * offset) / block.length, rate));
        flashbytesindex = bytes_after;
        phase.Update(static_cast<int>(flashbytesindex));

        if ((offset + kWriteChunkSize) % kCommitBlockSize == 0)
        {
            const std::uint32_t commit_start = address + kWriteChunkSize - kCommitBlockSize;
            const std::uint32_t crc = checksum::Crc32(image.subspan(commit_start, kCommitBlockSize));
            const std::uint8_t command = test_write ? 0x23 : 0x24;
            events.Log(LogLevel::kInfo, "Flash buffer write complete... ");
            events.Log(LogLevel::kDebug, std::format("Image CRC32: 0x{:x}", crc));
            events.Log(LogLevel::kInfo, test_write ? std::format("Validate flash addr: 0x{:x}", commit_start)
                                                   : std::format("Committ flash addr: 0x{:x}", commit_start));
            events.Log(LogLevel::kInfo, std::format(" len: 0x{:x}", kCommitBlockSize));
            events.Log(LogLevel::kInfo, std::format(" crc32: 0x{:x}", crc));
            const bytes::Bytes commit =
                IsoRequest(command, ComposeBe(commit_start, std::uint16_t{kCommitBlockSize}, crc));
            if (Status written = IsoWrite(transport, commit, cancellation, "flash commit"); !written)
            {
                return written;
            }
            Result<std::optional<bytes::Bytes>> committed =
                IsoRead(transport, kFlashTimeout, cancellation, "flash commit");
            if (!committed)
            {
                return std::unexpected(committed.error());
            }
            if (!committed->has_value())
            {
                return Fail(ErrorKind::kTimeout, "no response from ECU during flash commit");
            }
            if (Status valid =
                    ExpectIsoResponse(**committed, static_cast<std::uint8_t>(command | 0x40U), 0, "flash commit");
                !valid)
            {
                return valid;
            }
        }
    }
    return {};
}

void LogChangedBlocks(IEventSink& events, const std::vector<bool>& modified)
{
    unsigned count = 0;
    events.Log(LogLevel::kInfo, "Different blocks : ");
    for (std::size_t index = 0; index < modified.size(); ++index)
    {
        if (modified[index])
        {
            events.Log(LogLevel::kInfo, std::format("{}, ", index));
            ++count;
        }
    }
    events.Log(LogLevel::kInfo, std::format(" (total: {})", count));
}

Status WriteMem(IMixedCanFlashTransport& transport, const FlashPlan& plan, IClock& clock,
                const ICancellationToken& cancellation, IEventSink& events, PhaseSequence& phases,
                PhaseReporter& compare_phase)
{
    // Legacy write_mem()/get_changed_blocks(), lines 662-819, then
    // check_romcrc()/init_flash_write()/reflash_block()/flash_block(),
    // lines 820-1395. The portable plan retains every one of the sixteen
    // physical regions, so the image is addressed by its absolute flash
    // offsets exactly as legacy's data_array was.
    if (!plan.Image().has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN write requires a ROM image");
    }
    const FlashDevice *device = FindFlashDevice(plan.McuName());
    if (device == nullptr || device->numblocks != plan.EraseRegions().size())
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN flash geometry is unavailable");
    }
    const bytes::ByteView image = *plan.Image();
    const bool test_write = plan.Operation() == FlashOperation::kTestWrite;
    bool destructive_erase_succeeded = false;
    const auto propagate_after_erase = [&](const Error& error) -> Status
    {
        if (destructive_erase_succeeded)
        {
            events.Log(LogLevel::kError, kReflashRecoveryWarning);
        }
        return std::unexpected(error);
    };
    std::vector<bool> modified(device->numblocks, false);
    const auto compare_blocks = [&](PhaseReporter *progress) -> Result<unsigned>
    {
        unsigned count = 0;
        for (unsigned index = 0; index < device->numblocks; ++index)
        {
            if (Status cancelled = CheckCancelled(cancellation, "cancelled during ROM comparison"); !cancelled)
            {
                return std::unexpected(cancelled.error());
            }
            const MemoryRegion block{device->fblocks[index].start, device->fblocks[index].len};
            events.Log(LogLevel::kInfo, std::format("FB{:02}\t0x{:08X}\t0x{:08X}", index, block.start, block.length));
            Result<std::uint32_t> ecu_crc = ReadBlockCrc(transport, block, cancellation);
            if (!ecu_crc)
            {
                return std::unexpected(ecu_crc.error());
            }
            const std::uint32_t image_crc = checksum::Crc32(image.subspan(block.start, block.length));
            modified[index] = *ecu_crc != image_crc;
            events.Log(LogLevel::kDebug, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", *ecu_crc, image_crc));
            events.Log(LogLevel::kInfo, std::format("\t{:08X}\t{:08X}", *ecu_crc, image_crc));
            events.Log(LogLevel::kInfo, modified[index] ? "\tNO" : "\tYES");
            count += modified[index] ? 1U : 0U;
            if (progress != nullptr)
            {
                progress->Update(static_cast<int>(index + 1));
            }
            if (Status waited = clock.Sleep(kCrcComparisonDelay, cancellation); !waited)
            {
                return std::unexpected(waited.error());
            }
        }
        return count;
    };

    events.Log(LogLevel::kInfo, "--- Comparing ECU flash memory pages to image file ---");
    events.Log(LogLevel::kInfo, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    Result<unsigned> changed_count = compare_blocks(&compare_phase);
    if (!changed_count)
    {
        return std::unexpected(changed_count.error());
    }
    LogChangedBlocks(events, modified);
    if (*changed_count == 0)
    {
        events.Log(LogLevel::kInfo,
                   "*** Compare results no difference between ROM and ECU data, no flashing needed! ***");
        compare_phase.Complete();
        phases.Start(test_write ? "TestWrite" : "Write", 0);
        return {};
    }
    compare_phase.Complete();
    events.Log(LogLevel::kInfo, "--- Start writing ROM file to ECU flash memory ---");
    if (Status initialized = InitFlashWrite(transport, test_write, cancellation, events); !initialized)
    {
        return initialized;
    }

    std::uint32_t flashbytescount = 0;
    for (unsigned index = 0; index < device->numblocks; ++index)
    {
        if (modified[index])
        {
            flashbytescount += device->fblocks[index].len;
        }
    }
    auto write_phase = phases.Start(test_write ? "TestWrite" : "Write", static_cast<int>(flashbytescount));
    std::uint32_t flashbytesindex = 0;
    for (unsigned index = 0; index < device->numblocks; ++index)
    {
        if (!modified[index])
        {
            continue;
        }
        const MemoryRegion block{device->fblocks[index].start, device->fblocks[index].len};
        events.Log(LogLevel::kInfo, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
        if (Status voltage = QueryProgrammingVoltage(transport, cancellation, events); !voltage)
        {
            return propagate_after_erase(voltage.error());
        }
        if (Status flashed = FlashBlock(transport, image, block, test_write, clock, cancellation, events, write_phase,
                                        destructive_erase_succeeded, flashbytesindex, flashbytescount);
            !flashed)
        {
            return propagate_after_erase(flashed.error());
        }
        events.Log(LogLevel::kInfo, "Flash block ok");
        events.Log(LogLevel::kInfo, std::format("Block {} reflash complete.", index));
    }
    write_phase.Complete();

    events.Log(LogLevel::kInfo, "--- Comparing ECU flash memory pages to image file after reflash ---");
    events.Log(LogLevel::kInfo, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    Result<unsigned> remaining = compare_blocks(nullptr);
    if (!remaining)
    {
        return propagate_after_erase(remaining.error());
    }
    LogChangedBlocks(events, modified);
    if (test_write)
    {
        events.Log(LogLevel::kInfo, "*** Test write PASS, it's ok to perform actual write! ***");
    }
    else if (*remaining != 0)
    {
        events.Log(LogLevel::kError, "*** ERROR IN FLASH PROCESS ***");
        events.Log(LogLevel::kError,
                   "Don't power off your ECU, kernel is still running and you can try flashing again!");
    }
    return {};
}

} // namespace

Result<MixedCanConfig> SubaruDensoSh705xDensoCanExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (Status family = CheckFamily(plan, FlashFamily::kSubaruDensoSh705xDensoCan); !family)
    {
        return std::unexpected(family.error());
    }
    if (Status valid = ValidateSubaruDensoSh705xDensocanPlan(plan); !valid)
    {
        return std::unexpected(valid.error());
    }
    const auto& wire = std::get<SubaruDensoSh705xDensoCanPlan>(plan.FamilyPlan());
    return MixedCanConfig{
        .kernel = {.bitrate = wire.bitrate,
                   .request_id = wire.iso_request_id,
                   .response_id = wire.iso_response_id,
                   .extended_id = wire.iso_extended_id},
        .bootloader = {.bitrate = wire.bitrate,
                       .transmit_id = wire.raw_transmit_id,
                       .receive_id = wire.raw_receive_id,
                       .extended_id = wire.raw_extended_id},
    };
}

Status SubaruDensoSh705xDensoCanExecutor::BeforeTransportConfigure(IMixedCanFlashTransport& transport, IClock&,
                                                                   const ICancellationToken& cancellation) const
{
    if (Status cancelled = CheckCancelled(cancellation, "cancelled before mixed CAN reset"); !cancelled)
    {
        return cancelled;
    }
    if (const Status reset = transport.ResetConnection(); !reset)
    {
        return reset;
    }
    return CheckCancelled(cancellation, "cancelled after mixed CAN reset");
}

Status SubaruDensoSh705xDensoCanExecutor::BeforeTransportOpen(const ICancellationToken& cancellation) const
{
    if (Status cancelled = CheckCancelled(cancellation, "cancelled after mixed CAN configuration"); !cancelled)
    {
        return cancelled;
    }
    return CheckCancelled(cancellation, "cancelled before opening mixed CAN transport");
}

Result<FlashExecutionResult>
SubaruDensoSh705xDensoCanExecutor::Execute(const FlashPlan& plan, IMixedCanFlashTransport& transport, IClock& clock,
                                           const ICancellationToken& cancellation, IEventSink& events)
{
    if (Status family = CheckFamily(plan, FlashFamily::kSubaruDensoSh705xDensoCan); !family)
    {
        return std::unexpected(family.error());
    }
    if (Status valid = ValidateSubaruDensoSh705xDensocanPlan(plan); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (Status cancelled = CheckCancelled(cancellation, "cancelled before DensoCAN execution"); !cancelled)
    {
        return std::unexpected(cancelled.error());
    }

    PhaseSequence phases(events, plan.Operation() == FlashOperation::kRead ? 2 : 4);
    auto kernel_phase = phases.Start("Kernel", 1);
    Result<bool> kernel_alive = ProbeKernel(transport, clock, cancellation, events);
    if (!kernel_alive)
    {
        return std::unexpected(kernel_alive.error());
    }
    if (!*kernel_alive)
    {
        if (Status raw_mode = transport.EnterRawBootloaderMode(); !raw_mode)
        {
            return std::unexpected(raw_mode.error());
        }
        if (Status woken = WakeBootloader(transport, clock, cancellation, events); !woken)
        {
            return std::unexpected(woken.error());
        }
        if (!plan.Kernel().has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "DensoCAN plan lacks the required kernel image");
        }
        if (Status uploaded = UploadKernel(transport, *plan.Kernel(), clock, cancellation, events); !uploaded)
        {
            return std::unexpected(uploaded.error());
        }
        if (Status iso_mode = transport.EnterIso15765KernelMode(); !iso_mode)
        {
            return std::unexpected(iso_mode.error());
        }
        if (Status kernel_id = RequireKernelId(transport, clock, cancellation, events); !kernel_id)
        {
            return std::unexpected(kernel_id.error());
        }
    }
    kernel_phase.Complete();

    if (plan.Operation() == FlashOperation::kRead)
    {
        auto read_phase = phases.Start("Read", static_cast<int>(plan.TransferRegion().length));
        Result<bytes::Bytes> read = ReadMem(transport, plan.TransferRegion(), clock, cancellation, events, read_phase);
        if (!read)
        {
            return std::unexpected(read.error());
        }
        return FlashExecutionResult{
            .operation = plan.Operation(), .read_bytes = std::move(*read), .rom_id = std::nullopt};
    }

    auto compare_phase = phases.Start("Compare", 16);
    if (Status written = WriteMem(transport, plan, clock, cancellation, events, phases, compare_phase); !written)
    {
        return std::unexpected(written.error());
    }
    auto complete_phase = phases.Start("Complete", 1);
    complete_phase.Complete();
    return FlashExecutionResult{.operation = plan.Operation(), .read_bytes = std::nullopt, .rom_id = std::nullopt};
}

} // namespace fastecu::flash
