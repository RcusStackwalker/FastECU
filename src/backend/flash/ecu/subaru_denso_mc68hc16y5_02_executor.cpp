#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_error.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/ecu/denso_beef_kline_common.h"
#include "src/backend/flash/ecu/plan_primitives.h"
#include "src/backend/flash/flash_device_lookup.h"
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

constexpr std::uint32_t kReadPageSize = 0x400;
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitBlockSize = 0x1000;

Result<bytes::Bytes> ExchangeImpl(IKlineFlashTransport& transport, IClock *clock,
                                  const ICancellationToken& cancellation, bytes::ByteView request,
                                  std::chrono::milliseconds settle, std::chrono::milliseconds timeout)
{
    Result<IKlineFlashTransport::OptionalBytes> received =
        DensoBeefKlineExchangeOptional(transport, clock, cancellation, request, settle, timeout);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no response from ECU");
    }
    return std::move(**received);
}

Result<bytes::Bytes> Exchange(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              bytes::ByteView request, std::chrono::milliseconds settle,
                              std::chrono::milliseconds timeout)
{
    return ExchangeImpl(transport, &clock, cancellation, request, settle, timeout);
}

Result<bytes::Bytes> Exchange(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                              bytes::ByteView request, std::chrono::milliseconds timeout)
{
    return ExchangeImpl(transport, nullptr, cancellation, request, 0ms, timeout);
}

Result<IKlineFlashTransport::OptionalBytes> ExchangeOptional(IKlineFlashTransport& transport, IClock& clock,
                                                             const ICancellationToken& cancellation,
                                                             bytes::ByteView request, std::chrono::milliseconds settle,
                                                             std::chrono::milliseconds timeout)
{
    return DensoBeefKlineExchangeOptional(transport, &clock, cancellation, request, settle, timeout);
}

Status DrainResponse(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                     std::chrono::milliseconds timeout, std::string_view detail)
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

Status ChangeBaud(IKlineFlashTransport& transport, const ICancellationToken& cancellation, int baud)
{
    return DensoBeefKlineGuarded(cancellation, "changing baud", [&] { return transport.SetBaud(baud); });
}

// frame a kernel-ID probe, settle 200ms, then read with the long timeout.
// A no-frame response means the kernel is not alive yet, not an I/O failure.
Result<bytes::Bytes> RequestKernelId(IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation)
{
    const bytes::Bytes request = DensoBeefKlineFrame(kDensoKlineOpId);
    Result<IKlineFlashTransport::OptionalBytes> received =
        ExchangeOptional(transport, clock, cancellation, request, 200ms, 2000ms);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    return received->has_value() ? std::move(**received) : bytes::Bytes{};
}

bool LooksKernelAlive(bytes::ByteView received)
{
    return DensoBeefKlineResponseOk(received, static_cast<bytes::Byte>(kDensoKlineOpId | 0x40U));
}

// discard any stale serial bytes after disabling LEC lines and before the
// boot-entry sequence. Empty and non-empty reads are both successful drains.
Status DrainInitialResponse(IKlineFlashTransport& transport, const ICancellationToken& cancellation)
{
    return DrainResponse(transport, cancellation, 10ms, "initial drain");
}

// 287-317 and 1139-1168: at 62500 baud, the kernel answers its ID probe.
Status ExpectKernelAlive(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation)
{
    return ChangeBaud(transport, cancellation, 62500)
        .and_then([&] { return RequestKernelId(transport, clock, cancellation); })
        .and_then(
            [](const bytes::Bytes& probe) -> Status
            {
                if (probe.size() <= 4)
                {
                    return Fail(ErrorKind::kBadResponse, "No valid response from ECU");
                }
                if (!LooksKernelAlive(probe))
                {
                    return Fail(ErrorKind::kBadResponse, "Wrong response from ECU");
                }
                return {};
            });
}

bool BootloaderAnswered(const IKlineFlashTransport::OptionalBytes& response,
                        const SubaruDensoMc68hc16y5_02Plan& family_plan)
{
    return response.has_value() && response->size() >= family_plan.bootloader_ok.size() &&
           std::equal(family_plan.bootloader_ok.begin(), family_plan.bootloader_ok.end(), response->begin());
}

// pad to 16 bytes, encrypt every byte, replace encrypted[2..3] with the
// kernel magic, then frame it raw, not SUB_KERNEL_START_COMM-framed.
bytes::Bytes KernelUploadRequest(const KernelImage& kernel, const SubaruDensoMc68hc16y5_02Plan& family_plan)
{
    bytes::Bytes payload = kernel.bytes;
    payload.resize((payload.size() + 0x0F) & ~std::size_t{0x0F}, 0x00);
    for (auto& byte : payload)
    {
        byte = static_cast<bytes::Byte>((byte ^ family_plan.encryption_xor) + 0x10);
    }
    bytes::WriteU16Be(bytes::MutableByteView(payload), 2, family_plan.kernel_magic);
    // Not a full u24(address): the frame carries only the address's high two
    // bytes here (bits 23-8). The low byte (bits 7-0) is never emitted in
    // this header.
    return ComposeBeWithChecksum(&fastecu::checksum::NegatedSum8, kDensoKlineOpUploadKernel,
                                 std::uint16_t(kernel.load_address >> 8U),
                                 U24{static_cast<std::uint32_t>(payload.size())}, payload);
}

// up to twenty 50ms reads, paced by 100ms, until the 10-byte reply is whole.
Status ReadCrcContinuation(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                           bytes::Bytes& response)
{
    for (int try_count = 0; response.size() < 10 && try_count < 20; ++try_count)
    {
        if (Status cancelled = CheckCancelled(cancellation, "cancelled before CRC continuation read");
            !cancelled.has_value())
        {
            return cancelled;
        }
        Result<IKlineFlashTransport::OptionalBytes> more = transport.Read(50ms, cancellation);
        if (!more.has_value())
        {
            return std::unexpected(more.error());
        }
        if (more->has_value())
        {
            response.append_range(bytes::ByteView(**more).first(std::min(10 - response.size(), (**more).size())));
        }
        if (Status slept = clock.Sleep(100ms, cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    return {};
}

// The flash device's span placed at ECU addresses, as the legacy write path's
// padded buffer held it
// ADR 0020). Only flash block ranges are compared and written, so what the
// span holds between them (the RAM range) is never sent.
Result<bytes::Bytes> AddressedImage(const memory::MemoryImage& image, std::span<const MemoryRegion> blocks)
{
    const std::uint32_t span_start = blocks.front().start;
    std::uint32_t span_end = span_start;
    for (const MemoryRegion& block : blocks)
    {
        span_end = std::max(span_end, block.start + block.length);
    }
    const auto span = memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{span_start},
                                                                     memory::ByteCount{span_end - span_start});
    const auto rendered =
        span.has_value() ? image.Render(*span) : std::unexpected(memory::MemoryError{.detail = "empty flash device"});
    if (!rendered.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("ROM image does not cover the flash device: {}", rendered.error().detail));
    }
    bytes::Bytes addressed_image(span_end, 0xFF);
    std::ranges::copy(rendered->Data(), addressed_image.begin() + span_start);
    return addressed_image;
}

// Legacy init_flash_write(), full exchanges at
Status InitFlashWrite(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                      bool test_write)
{
    const std::uint8_t enable_opcode = test_write ? kDensoKlineOpFlashDisable : kDensoKlineOpFlashEnable;
    for (const auto& [opcode, more_than] :
         {std::pair{kDensoKlineOpGetMaxMsgSize, std::size_t{9}},
          std::pair{kDensoKlineOpGetMaxBlockSize, std::size_t{9}}, std::pair{enable_opcode, std::size_t{0}}})
    {
        Result<bytes::Bytes> response =
            Exchange(transport, clock, cancellation, DensoBeefKlineFrame(opcode), 200ms, 200ms);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (Status ack = DensoBeefKlineCheckAck(*response, opcode, "flash init", more_than); !ack.has_value())
        {
            return ack;
        }
    }
    return {};
}

// One exchange whose reply must acknowledge `opcode`.
Status ExchangeAck(Result<bytes::Bytes> response, bytes::Byte opcode, std::string_view phase, std::size_t more_than = 0)
{
    return response.and_then([&](const bytes::Bytes& reply)
                             { return DensoBeefKlineCheckAck(reply, opcode, phase, more_than); });
}

} // namespace

Status SubaruDensoMc68hc16y5_02Executor::ConnectBootloader(IKlineFlashTransport& transport, IClock& clock,
                                                           const ICancellationToken& cancellation, IEventSink& events,
                                                           const SubaruDensoMc68hc16y5_02Plan& family_plan,
                                                           bool& kernel_alive) const
{
    Result<IKlineFlashTransport::OptionalBytes> init_response =
        CheckCancelled(cancellation, "cancelled before connect")
            // Legacy
            .and_then([&] { return clock.Sleep(200ms, cancellation); })
            .and_then(
                [&]
                {
                    return DensoBeefKlineGuarded(cancellation, "LEC pulse",
                                                 [&] { return transport.PulseLec2Line(200ms); });
                })
            .and_then([&] { return clock.Sleep(200ms, cancellation); })
            .and_then(
                [&]
                {
                    events.Log(LogLevel::kInfo, "Connecting to Subaru 01-05 16-bit K-Line bootloader...");
                    const bytes::Bytes init_request{0x4D, 0xFF, 0xB4};
                    return ExchangeOptional(transport, clock, cancellation, init_request, 50ms, 200ms);
                });
    if (!init_response.has_value())
    {
        return std::unexpected(init_response.error());
    }
    if (BootloaderAnswered(*init_response, family_plan))
    {
        events.Log(LogLevel::kInfo, "Connected to bootloader");
        kernel_alive = false;
        return {};
    }

    events.Log(LogLevel::kWarning, "Bad response from bootloader, checking for a running kernel...");
    return clock.Sleep(100ms, cancellation)
        .and_then(
            [&]
            {
                return DensoBeefKlineGuarded(cancellation, "disabling LEC lines",
                                             [&] { return transport.DisableLecLines(); });
            })
        .and_then([&] { return ExpectKernelAlive(transport, clock, cancellation); })
        .transform(
            [&]
            {
                kernel_alive = true;
                events.Log(LogLevel::kInfo, "Kernel already running");
            });
}

Status SubaruDensoMc68hc16y5_02Executor::UploadKernel(IKlineFlashTransport& transport, IClock& clock,
                                                      const ICancellationToken& cancellation, IEventSink& events,
                                                      const SubaruDensoMc68hc16y5_02Plan& family_plan,
                                                      const KernelImage& kernel) const
{
    return CheckCancelled(cancellation, "cancelled before kernel upload")
        .and_then(
            [&]() -> Status
            {
                if (kernel.bytes.empty())
                {
                    return Fail(ErrorKind::kInvalidConfig, "kernel image is empty");
                }
                return ChangeBaud(transport, cancellation, family_plan.kernel_baud);
            })
        .and_then(LogThen(events, "Sending kernel...",
                          [&]
                          {
                              return ExchangeOptional(transport, clock, cancellation,
                                                      KernelUploadRequest(kernel, family_plan), 0ms, 200ms);
                          }))
        .and_then(
            // Legacy read_serial_data() returns an empty QByteArray when no frame
            // arrives. That no-frame outcome is the upload success signal; a real
            // frame, including a present-but-empty frame, is an ECU error response.
            [](const IKlineFlashTransport::OptionalBytes& upload_response) -> Status
            {
                if (upload_response.has_value())
                {
                    return Fail(ErrorKind::kBadResponse, "Error on kernel upload");
                }
                return {};
            })
        .and_then(LogThen(events, "Kernel uploaded successfully", [&] { return clock.Sleep(1500ms, cancellation); }))
        .and_then([&] { return ExpectKernelAlive(transport, clock, cancellation); })
        .transform([&] { events.Log(LogLevel::kInfo, "Kernel is alive"); });
}

Result<bytes::Bytes> SubaruDensoMc68hc16y5_02Executor::ReadMem(IKlineFlashTransport& transport, IClock& clock,
                                                               const ICancellationToken& cancellation,
                                                               IEventSink& events, const std::string& mcu_name) const
{
    // Legacy jumps the wire address across the RAM/kernel hole
    // but pad desktop FullRomData with 0xff there. The portable boundary uses
    // a packed ROM instead, so concatenate real flash blocks and omit the hole.
    const FlashDevice *device = FindFlashDevice(mcu_name);
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "Unknown MCU type");
    }
    bytes::Bytes mapdata;
    mapdata.reserve(device->romsize);
    events.Progress(0, static_cast<int>(device->romsize));
    std::uint32_t packed_remaining = device->romsize;

    for (unsigned block_no = 0; block_no < device->numblocks && packed_remaining > 0; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        const std::uint32_t block_bytes = std::min(block.len, packed_remaining);
        if (block_bytes % kReadPageSize != 0)
        {
            return Fail(ErrorKind::kInvalidConfig, "flash block is not page aligned");
        }
        const std::uint32_t block_end = block.start + block_bytes;
        for (std::uint32_t address = block.start; address < block_end; address += kReadPageSize)
        {
            if (Status cancelled = CheckCancelled(cancellation, "cancelled during read"); !cancelled.has_value())
            {
                return std::unexpected(cancelled.error());
            }
            const auto payload = ComposeBe(0x00_b, U24{address}, std::uint16_t(kReadPageSize));
            Result<bytes::Bytes> response = Exchange(transport, clock, cancellation,
                                                     DensoBeefKlineFrame(kDensoKlineOpReadArea, payload), 10ms, 3000ms);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (response->size() != kReadPageSize + 6 ||
                !DensoBeefKlineResponseOk(*response, static_cast<bytes::Byte>(kDensoKlineOpReadArea | 0x40U)))
            {
                return Fail(ErrorKind::kBadResponse, "Wrong response from ECU during read");
            }
            mapdata.insert(mapdata.end(), response->begin() + 5, response->end() - 1);
            packed_remaining -= kReadPageSize;
            events.Progress(static_cast<int>(mapdata.size()), static_cast<int>(device->romsize));
            if (Status slept = clock.Sleep(1ms, cancellation); !slept.has_value())
            {
                return std::unexpected(slept.error());
            }
        }
    }
    if (mapdata.size() != device->romsize)
    {
        return Fail(ErrorKind::kInvalidConfig, "flash blocks do not match ROM size");
    }
    events.Progress(static_cast<int>(device->romsize), static_cast<int>(device->romsize));
    return mapdata;
}

Result<std::uint32_t> SubaruDensoMc68hc16y5_02Executor::ReadBlockCrc(IKlineFlashTransport& transport, IClock& clock,
                                                                     const ICancellationToken& cancellation,
                                                                     const MemoryRegion& block) const
{
    // request the ECU's CRC over [start, start + length).
    const Result<std::uint32_t> crc =
        DensoBeefKlineRequestBlockCrc(transport, cancellation, block)
            .and_then(
                [&](bytes::Bytes response)
                {
                    return ReadCrcContinuation(transport, clock, cancellation, response)
                        .and_then([&] { return DensoBeefKlineCheckAck(response, kDensoKlineOpCrc, "CRC check", 9); })
                        .transform([&] { return bytes::ReadU32Be(response, 5); });
                });
    if (!crc.has_value())
    {
        return crc;
    }
    return DrainResponse(transport, cancellation, 200ms, "CRC response drain").transform([&] { return *crc; });
}

Status SubaruDensoMc68hc16y5_02Executor::FlashBlock(IKlineFlashTransport& transport, IClock& clock,
                                                    const ICancellationToken& cancellation, IEventSink& events,
                                                    bytes::ByteView image, const MemoryRegion& block,
                                                    bool test_write) const
{
    if (block.start > image.size() || block.length > image.size() - block.start ||
        block.length % kWriteChunkSize != 0 || block.length % kCommitBlockSize != 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "flash block is not represented by the image");
    }
    if (!test_write)
    {
        // Legacy
        events.Log(LogLevel::kInfo, "Erasing flash page...");
        if (Status erased = ExchangeAck(Exchange(transport, clock, cancellation,
                                                 DensoBeefKlineFrame(kDensoKlineOpBlankPage, ComposeBe(block.start)),
                                                 500ms, 3000ms),
                                        kDensoKlineOpBlankPage, "erase");
            !erased.has_value())
        {
            return erased;
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
        // Legacy
        const std::uint32_t chunk_address = block.start + offset;
        const bytes::Bytes payload =
            DensoBeefKlineWritePayload(chunk_address, image.subspan(chunk_address, kWriteChunkSize));
        if (Status written = ExchangeAck(
                Exchange(transport, cancellation, DensoBeefKlineFrame(kDensoKlineOpWriteFlashBuffer, payload), 3000ms),
                kDensoKlineOpWriteFlashBuffer, "write");
            !written.has_value())
        {
            return written;
        }
        offset += kWriteChunkSize;
        if (offset % kCommitBlockSize == 0)
        {
            // Legacy
            const std::uint32_t commit_start = block.start + offset - kCommitBlockSize;
            const auto commit_payload =
                ComposeBe(commit_start, std::uint16_t(kCommitBlockSize),
                          fastecu::checksum::Crc32(image.subspan(commit_start, kCommitBlockSize)));
            if (Status committed =
                    ExchangeAck(Exchange(transport, clock, cancellation,
                                         DensoBeefKlineFrame(commit_opcode, commit_payload), 200ms, 3000ms),
                                commit_opcode, "commit");
                !committed.has_value())
            {
                return committed;
            }
        }
        events.Progress(static_cast<int>(offset), static_cast<int>(block.length));
    }
    return DrainResponse(transport, cancellation, 200ms, "flash block drain");
}

Status SubaruDensoMc68hc16y5_02Executor::WriteMem(IKlineFlashTransport& transport, IClock& clock,
                                                  const ICancellationToken& cancellation, IEventSink& events,
                                                  const memory::MemoryImage& image, const std::string& mcu_name,
                                                  bool test_write) const
{
    const FlashDevice *device = FindFlashDevice(mcu_name);
    if (device == nullptr || device->numblocks == 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "Unknown MCU type");
    }
    const std::vector<MemoryRegion> blocks = detail::MakeEraseRegions(*device);
    return AddressedImage(image, blocks)
        .and_then(
            [&](const bytes::Bytes& addressed_image)
            {
                return DensoBeefKlineWriteChangedBlocks(
                    transport, cancellation, events, blocks, addressed_image, test_write,
                    [&](const MemoryRegion& block) { return ReadBlockCrc(transport, clock, cancellation, block); },
                    [&] { return InitFlashWrite(transport, clock, cancellation, test_write); },
                    [&](const MemoryRegion& block)
                    { return FlashBlock(transport, clock, cancellation, events, addressed_image, block, test_write); });
            });
}

Result<KlineConfig> SubaruDensoMc68hc16y5_02Executor::Setup(const SubaruDensoMc68hc16y5_02Plan& family) const
{
    return KlineConfig{.baud = family.connect_baud, .iso14230 = false, .tester_id = 0, .target_id = 0};
}

Status SubaruDensoMc68hc16y5_02Executor::BeforeTransportOpen(const ICancellationToken& cancellation) const
{
    return CheckCancelled(cancellation, "cancelled after transport configuration");
}

Result<FlashExecutionResult> SubaruDensoMc68hc16y5_02Executor::Run(const SubaruDensoMc68hc16y5_02Plan& family_plan,
                                                                   const FlashPlan& plan, IKlineFlashTransport& kline,
                                                                   IClock& clock,
                                                                   const ICancellationToken& cancellation,
                                                                   IEventSink& events)
{
    bool kernel_alive = false;
    // plan admission is the cancellation checkpoint before the LEC lines.
    if (Status connected =
            kline.DisableLecLines()
                .and_then([&] { return CheckCancelled(cancellation, "cancelled after disabling LEC lines"); })
                .and_then([&] { return DrainInitialResponse(kline, cancellation); })
                .and_then([&]
                          { return ConnectBootloader(kline, clock, cancellation, events, family_plan, kernel_alive); });
        !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    if (!kernel_alive)
    {
        if (!plan.Kernel().has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5_02 requires a kernel image");
        }
        if (Status uploaded = UploadKernel(kline, clock, cancellation, events, family_plan, *plan.Kernel());
            !uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
    }
    if (plan.Operation() == FlashOperation::kRead)
    {
        Result<bytes::Bytes> read = ReadMem(kline, clock, cancellation, events, plan.McuName());
        if (!read.has_value())
        {
            return std::unexpected(read.error());
        }
        return FlashExecutionResult{.operation = plan.Operation(), .read_bytes = std::move(*read)};
    }

    const std::optional<memory::MemoryImage>& image = plan.RomImage();
    if (!image.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5_02 write requires a ROM image");
    }
    if (Status written = WriteMem(kline, clock, cancellation, events, *image, plan.McuName(),
                                  plan.Operation() == FlashOperation::kTestWrite);
        !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{.operation = plan.Operation(), .read_bytes = std::nullopt};
}

} // namespace fastecu::flash
