#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/testing/scripted_mixed_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/mock_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

using ::testing::_;
using ::testing::AtLeast;
using ::testing::Contains;
using ::testing::DoAll;
using ::testing::ElementsAre;

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;

constexpr std::uint32_t kIsoRequestId = 0x7E0;
constexpr std::uint32_t kIsoResponseId = 0x7E8;
constexpr std::uint32_t kRawTransmitId = 0x000FFFFE;
constexpr std::uint32_t kRawReceiveId = 0x21;
constexpr std::uint32_t kReadPageSize = 0x400;
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitBlockSize = 0x1000;
constexpr std::array<bytes::Byte, 8> kRawReadFirstWireBytes{0xD3, 0x5A, 0xC7, 0x19, 0x2E, 0xF4, 0x80, 0x6B};
constexpr std::array<bytes::Byte, 8> kRawReadLastWireBytes{0x9C, 0x31, 0xE7, 0x04, 0xB2, 0x6D, 0x58, 0xAF};
constexpr std::array<bytes::Byte, 8> kCallerFirstCommitBytes{0x41, 0x9D, 0xE3, 0x27, 0xB8, 0x06, 0xCA, 0x5F};
constexpr std::array<bytes::Byte, 8> kCallerSecondCommitBytes{0x72, 0x0C, 0xF1, 0x96, 0x3B, 0xD4, 0x58, 0xAE};
constexpr std::array<bytes::Byte, 8> kExpectedFirstCommitWireBytes{0x41, 0x9D, 0xE3, 0x27, 0xB8, 0x06, 0xCA, 0x5F};
constexpr std::array<bytes::Byte, 8> kExpectedSecondCommitWireBytes{0x72, 0x0C, 0xF1, 0x96, 0x3B, 0xD4, 0x58, 0xAE};

// Keeps the existing strict scripted transport while recording the timeout
// used for each BEEF read. The executor owns all protocol behavior; this is
// only an observation seam for legacy timing assertions.
class TimeoutRecordingMixedCanTransport final : public IMixedCanFlashTransport
{
  public:
    Status ResetConnection() override
    {
        lifecycle.push_back("reset_connection");
        Status result = reset_result;
        if (result.has_value() && cancellation_on_reset != nullptr)
        {
            cancellation_on_reset->SetCancelled(true);
        }
        return result;
    }
    Status Configure(const MixedCanConfig& config) override
    {
        lifecycle.push_back("configure");
        return scripted.Configure(config);
    }
    Status Open() override
    {
        lifecycle.push_back("open");
        Status result = scripted.Open();
        if (result.has_value() && cancellation_on_open != nullptr)
        {
            cancellation_on_open->SetCancelled(true);
        }
        return result;
    }
    Status Close() override
    {
        lifecycle.push_back("close");
        return scripted.Close();
    }
    Status EnterRawBootloaderMode() override
    {
        return scripted.EnterRawBootloaderMode();
    }
    Status ClearReceiveBuffer() override
    {
        return scripted.ClearReceiveBuffer();
    }
    Status EnterIso15765KernelMode() override
    {
        return scripted.EnterIso15765KernelMode();
    }
    Status WriteIso15765(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        last_iso_opcode_ = data.size() > 8 ? std::optional<std::uint8_t>{data[8]} : std::nullopt;
        return scripted.WriteIso15765(data, cancellation);
    }
    Result<std::optional<bytes::Bytes>> ReadIso15765(std::chrono::milliseconds timeout,
                                                     const ICancellationToken& cancellation) override
    {
        iso_read_timeouts.emplace_back(last_iso_opcode_, timeout);
        return scripted.ReadIso15765(timeout, cancellation);
    }
    Status WriteRaw(const cdbg::CanFrame& frame, const ICancellationToken& cancellation) override
    {
        return scripted.WriteRaw(frame, cancellation);
    }
    Result<std::optional<cdbg::CanFrame>> ReadRaw(std::chrono::milliseconds timeout,
                                                  const ICancellationToken& cancellation) override
    {
        return scripted.ReadRaw(timeout, cancellation);
    }
    void RequestUnblock() noexcept override
    {
        scripted.RequestUnblock();
    }

    ScriptedMixedCanFlashTransport scripted;
    Status reset_result;
    std::vector<std::string> lifecycle;
    std::vector<std::pair<std::optional<std::uint8_t>, std::chrono::milliseconds>> iso_read_timeouts;
    FakeCancellationToken *cancellation_on_reset = nullptr;
    FakeCancellationToken *cancellation_on_open = nullptr;

  private:
    std::optional<std::uint8_t> last_iso_opcode_;
};

class CancellingEventSink final : public RecordingEventSink
{
  public:
    explicit CancellingEventSink(FakeCancellationToken& cancellation) : cancellation_(cancellation)
    {
    }

    void Progress(int done, int total) override
    {
        RecordingEventSink::Progress(done, total);
        if (done > 0)
        {
            cancellation_.SetCancelled(true);
        }
    }

    void PhaseProgress(const PhaseProgressEvent& event) override
    {
        phase_progress_calls.push_back(
            {std::string(event.phase_name), event.phase_index, event.phase_count, event.done, event.total});
        if ((event.phase_name == "Write" || event.phase_name == "TestWrite") && event.done > 0)
        {
            cancellation_.SetCancelled(true);
        }
    }

  private:
    FakeCancellationToken& cancellation_;
};

struct Case
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t rom_size;
    std::uint32_t kernel_address;
};

constexpr std::array<Case, 5> kCases{{
    {"sub_ecu_denso_sh7055_densocan", "SH7055", 0x00080000, 0xFFFF6004},
    {"sub_ecu_denso_sh7058_densocan", "SH7058", 0x00100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7058s_densocan", "SH7058", 0x00100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7058s_diesel_densocan", "SH7058", 0x00100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7059_diesel_densocan", "SH7059d", 0x00180000, 0xFFFEE000},
}};

KernelImage KernelFor(const Case& test_case, bytes::Bytes data = {0x11, 0x22, 0x33, 0x44, 0x55})
{
    return {.id = "densocan-kernel", .load_address = test_case.kernel_address, .bytes = std::move(data)};
}

Result<FlashPlan> ReadPlan(const Case& test_case, bytes::Bytes kernel = {0x11, 0x22, 0x33, 0x44, 0x55})
{
    return BuildSubaruDensoSh705xDensocanPlan(FlashOperation::kRead, test_case.protocol, test_case.mcu, std::nullopt,
                                              KernelFor(test_case, std::move(kernel)));
}

Result<FlashPlan> WritePlan(const Case& test_case, FlashOperation operation = FlashOperation::kWrite,
                            bytes::Bytes image = {})
{
    if (image.empty())
    {
        image.assign(test_case.rom_size, bytes::Byte{0});
    }
    return BuildSubaruDensoSh705xDensocanPlan(operation, test_case.protocol, test_case.mcu, std::move(image),
                                              KernelFor(test_case));
}

bytes::Bytes IsoRequest(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes message = ComposeBe(kIsoRequestId, std::uint16_t{0xBEEF},
                                     static_cast<std::uint16_t>(payload.size() + 1), bytes::Byte(opcode));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

bytes::Bytes KernelIdRequest()
{
    // Legacy request_kernel_id(), lines 1435-1484. The declared BEEF length
    // is one opcode byte, followed by three fixed zero bytes on the CAN wire.
    return {0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
}

bytes::Bytes IsoResponse(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes message = ComposeBe(kIsoResponseId, std::uint16_t{0xBEEF},
                                     static_cast<std::uint16_t>(payload.size() + 1), bytes::Byte(opcode));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

bytes::Bytes KernelIdResponse()
{
    return IsoResponse(0x41, bytes::Bytes{'K', 'I', 'D'});
}

cdbg::CanFrame RawRequest(bytes::Bytes payload)
{
    return {.id = kRawTransmitId, .payload = std::move(payload)};
}

cdbg::CanFrame RawResponse(bytes::Bytes payload, std::uint32_t id = kRawReceiveId)
{
    return {.id = id, .payload = std::move(payload)};
}

void ConfigureAndOpen(SubaruDensoSh705xDensoCanExecutor& executor, const FlashPlan& plan,
                      ScriptedMixedCanFlashTransport& transport)
{
    auto setup = executor.TransportSetup(plan);
    ASSERT_TRUE(setup.has_value()) << setup.error().detail;
    ASSERT_TRUE(transport.Configure(*setup).has_value());
    ASSERT_TRUE(transport.Open().has_value());
}

void ScriptKernelAlive(ScriptedMixedCanFlashTransport& transport)
{
    transport.ExpectIsoWrite(KernelIdRequest());
    transport.QueueIsoRead(KernelIdResponse());
}

void ScriptReadPages(ScriptedMixedCanFlashTransport& transport, std::uint32_t size, bytes::Byte wire_fill = 0)
{
    for (std::uint32_t address = 0; address < size; address += kReadPageSize)
    {
        transport.ExpectIsoWrite(IsoRequest(0x03, ComposeBe(0x00_b, U24(address), std::uint16_t{kReadPageSize})));
        transport.QueueIsoRead(IsoResponse(0x43, bytes::Bytes(kReadPageSize, wire_fill)));
    }
}

void ScriptRawReadPagesWithBoundarySentinels(ScriptedMixedCanFlashTransport& transport, std::uint32_t size)
{
    for (std::uint32_t address = 0; address < size; address += kReadPageSize)
    {
        transport.ExpectIsoWrite(IsoRequest(0x03, ComposeBe(0x00_b, U24(address), std::uint16_t{kReadPageSize})));
        bytes::Bytes page(kReadPageSize, bytes::Byte{0});
        if (address == 0)
        {
            std::copy(kRawReadFirstWireBytes.begin(), kRawReadFirstWireBytes.end(), page.begin());
        }
        if (address + kReadPageSize == size)
        {
            std::copy(kRawReadLastWireBytes.begin(), kRawReadLastWireBytes.end(),
                      page.end() - kRawReadLastWireBytes.size());
        }
        transport.QueueIsoRead(IsoResponse(0x43, page));
    }
}

void ScriptLiveRead(ScriptedMixedCanFlashTransport& transport, const Case& test_case, bytes::Byte wire_fill = 0)
{
    ScriptKernelAlive(transport);
    ScriptReadPages(transport, test_case.rom_size, wire_fill);
}

void ScriptUpload(ScriptedMixedCanFlashTransport& transport, std::optional<cdbg::CanFrame> jump_response = std::nullopt)
{
    for (int count = 0; count < 1000; ++count)
    {
        transport.ExpectRawWrite(RawRequest({0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    }
    transport.ExpectRawWrite(RawRequest({0x7A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.QueueRawRead(RawResponse({0x7A, 0x96, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    transport.ExpectRawWrite(RawRequest({0x7A, 0x9C, 0xFF, 0xFF, 0x60, 0x04, 0x00, 0x00}));
    transport.QueueRawRead(RawResponse({0x7A, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    // Hand-derived from the five-byte test kernel in legacy upload_kernel()
    // (r59f4e442 lines 227-507): its final six-byte block is zero-padded
    // and B4 carries FFFF6004 + 6 + 1 = FFFF600B. Keep these as fixed wire
    // literals; no test helper recomputes the production padding arithmetic.
    transport.ExpectRawWrite(RawRequest({0x7A, 0xAE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00}));
    transport.ExpectRawWrite(RawRequest({0x7A, 0xB4, 0xFF, 0xFF, 0x60, 0x0B, 0x00, 0x00}));
    transport.QueueRawRead(RawResponse({0x7A, 0xB1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    transport.ExpectRawWrite(RawRequest({0x7A, 0x9C, 0xFF, 0xFF, 0x60, 0x04, 0x00, 0x00}));
    transport.QueueRawRead(RawResponse({0x7A, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.ExpectRawWrite(RawRequest({0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    if (jump_response.has_value())
    {
        transport.QueueRawRead(std::move(*jump_response));
    }
    else
    {
        transport.QueueNoRawFrame(); // Legacy jump acknowledgement is tolerated.
    }
}

bytes::Bytes FixedRawZeroBytes(std::size_t size)
{
    return bytes::Bytes(size, bytes::Byte{0});
}

void ScriptCrc(ScriptedMixedCanFlashTransport& transport, const MemoryRegion& block, std::uint32_t crc)
{
    transport.ExpectIsoWrite(IsoRequest(0x02, ComposeBe(block.start, 0x00_b, U24(block.length))));
    transport.QueueIsoRead(IsoResponse(0x42, ComposeBe(crc)));
    // Legacy check_romcrc() discards a short response after either comparison result.
    transport.QueueNoIsoFrame();
}

std::uint32_t FixedRawZeroCrc(const MemoryRegion& block, bool mismatch)
{
    switch (block.length)
    {
    case 0x00001000:
        return mismatch ? 0xF722EF48U : 0xF722EF49U;
    case 0x00008000:
        return mismatch ? 0xE5FD2EA3U : 0xE5FD2EA2U;
    case 0x00010000:
        return mismatch ? 0xDFEA015CU : 0xDFEA015DU;
    default:
        ADD_FAILURE() << "missing hand-derived raw DensoCAN CRC fixture for block length " << block.length;
        return 0;
    }
}

void ScriptCompare(ScriptedMixedCanFlashTransport& transport, const FlashDevice& device, bool first_block_mismatches)
{
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion block{device.fblocks[index].start, device.fblocks[index].len};
        ScriptCrc(transport, block, FixedRawZeroCrc(block, first_block_mismatches && index == 0));
    }
}

void ScriptCompareWithModifiedBlocks(ScriptedMixedCanFlashTransport& transport, const FlashDevice& device,
                                     std::initializer_list<unsigned> modified_blocks)
{
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion block{device.fblocks[index].start, device.fblocks[index].len};
        const bool differs = std::find(modified_blocks.begin(), modified_blocks.end(), index) != modified_blocks.end();
        ScriptCrc(transport, block, FixedRawZeroCrc(block, differs));
    }
}

void ScriptRawImageCompare(ScriptedMixedCanFlashTransport& transport, const FlashDevice& device,
                           bool first_block_matches)
{
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion block{device.fblocks[index].start, device.fblocks[index].len};
        const std::uint32_t crc =
            index == 0 ? (first_block_matches ? 0xFF6A783EU : 0xFF6A783FU) : FixedRawZeroCrc(block, false);
        ScriptCrc(transport, block, crc);
    }
}

bytes::Bytes CallerRawDensocanImage(const Case& test_case)
{
    bytes::Bytes image(test_case.rom_size, bytes::Byte{0});
    std::copy(kCallerFirstCommitBytes.begin(), kCallerFirstCommitBytes.end(), image.begin());
    std::copy(kCallerSecondCommitBytes.begin(), kCallerSecondCommitBytes.end(), image.begin() + kWriteChunkSize);
    return image;
}

bytes::Bytes ExpectedRawCommitChunk(std::uint32_t offset)
{
    bytes::Bytes chunk(kWriteChunkSize, bytes::Byte{0});
    if (offset == 0)
    {
        std::copy(kExpectedFirstCommitWireBytes.begin(), kExpectedFirstCommitWireBytes.end(), chunk.begin());
    }
    if (offset == kWriteChunkSize)
    {
        std::copy(kExpectedSecondCommitWireBytes.begin(), kExpectedSecondCommitWireBytes.end(), chunk.begin());
    }
    return chunk;
}

void ScriptRawFirstFlashBlock(ScriptedMixedCanFlashTransport& transport, bool test_write)
{
    transport.ExpectIsoWrite(IsoRequest(0x04));
    transport.QueueIsoRead(IsoResponse(0x44, bytes::Bytes{0x00, 0x64}));
    if (!test_write)
    {
        transport.ExpectIsoWrite(IsoRequest(0x25, ComposeBe(std::uint32_t{0})));
        transport.QueueIsoRead(IsoResponse(0x65));
    }
    for (std::uint32_t offset = 0; offset < kCommitBlockSize; offset += kWriteChunkSize)
    {
        transport.ExpectIsoWrite(IsoRequest(0x22, ComposeBe(offset, ExpectedRawCommitChunk(offset))));
        transport.QueueIsoRead(IsoResponse(0x62));
    }
    transport.ExpectIsoWrite(IsoRequest(test_write ? 0x23 : 0x24,
                                        ComposeBe(std::uint32_t{0}, std::uint16_t{kCommitBlockSize}, 0xFF6A783EU)));
    transport.QueueIsoRead(IsoResponse(test_write ? 0x63 : 0x64));
}

void ScriptFlashInit(ScriptedMixedCanFlashTransport& transport, bool test_write)
{
    transport.ExpectIsoWrite(IsoRequest(0x05));
    transport.QueueIsoRead(IsoResponse(0x45, ComposeBe(std::uint32_t{0x00000200})));
    transport.ExpectIsoWrite(IsoRequest(0x06));
    transport.QueueIsoRead(IsoResponse(0x46, ComposeBe(std::uint32_t{0x00001000})));
    transport.ExpectIsoWrite(IsoRequest(test_write ? 0x21 : 0x20));
    transport.QueueIsoRead(IsoResponse(test_write ? 0x61 : 0x60));
}

void ScriptFirstFlashBlock(ScriptedMixedCanFlashTransport& transport, bool test_write)
{
    const std::uint32_t start = 0;
    // Legacy reflash_block() asks the programming-voltage question before
    // passing the block into flash_block(), which contains the erase.
    transport.ExpectIsoWrite(IsoRequest(0x04));
    transport.QueueIsoRead(IsoResponse(0x44, bytes::Bytes{0x00, 0x64}));
    if (!test_write)
    {
        transport.ExpectIsoWrite(IsoRequest(0x25, ComposeBe(start)));
        transport.QueueIsoRead(IsoResponse(0x65));
    }

    const bytes::Bytes raw_chunk = FixedRawZeroBytes(kWriteChunkSize);
    for (std::uint32_t offset = 0; offset < kCommitBlockSize; offset += kWriteChunkSize)
    {
        transport.ExpectIsoWrite(IsoRequest(0x22, ComposeBe(offset, raw_chunk)));
        transport.QueueIsoRead(IsoResponse(0x62));
    }
    transport.ExpectIsoWrite(
        IsoRequest(test_write ? 0x23 : 0x24, ComposeBe(start, std::uint16_t{kCommitBlockSize}, 0xF722EF49U)));
    transport.QueueIsoRead(IsoResponse(test_write ? 0x63 : 0x64));
}

void ScriptLargeNonzeroWriteBlock(ScriptedMixedCanFlashTransport& transport)
{
    struct FlashBufferExpectation
    {
        std::uint32_t address;
        bool commit_after;
    };
    // Fixed, hand-derived windows for SH7055 fblocks[8] (0x8000/0x8000).
    // The literal commit starts prove eight independent 0x1000 windows;
    // this fixture deliberately contains no payload transform or CRC logic.
    constexpr std::array<FlashBufferExpectation, 64> kBuffers{{
        {0x00008000, false}, {0x00008200, false}, {0x00008400, false}, {0x00008600, false}, {0x00008800, false},
        {0x00008A00, false}, {0x00008C00, false}, {0x00008E00, true},  {0x00009000, false}, {0x00009200, false},
        {0x00009400, false}, {0x00009600, false}, {0x00009800, false}, {0x00009A00, false}, {0x00009C00, false},
        {0x00009E00, true},  {0x0000A000, false}, {0x0000A200, false}, {0x0000A400, false}, {0x0000A600, false},
        {0x0000A800, false}, {0x0000AA00, false}, {0x0000AC00, false}, {0x0000AE00, true},  {0x0000B000, false},
        {0x0000B200, false}, {0x0000B400, false}, {0x0000B600, false}, {0x0000B800, false}, {0x0000BA00, false},
        {0x0000BC00, false}, {0x0000BE00, true},  {0x0000C000, false}, {0x0000C200, false}, {0x0000C400, false},
        {0x0000C600, false}, {0x0000C800, false}, {0x0000CA00, false}, {0x0000CC00, false}, {0x0000CE00, true},
        {0x0000D000, false}, {0x0000D200, false}, {0x0000D400, false}, {0x0000D600, false}, {0x0000D800, false},
        {0x0000DA00, false}, {0x0000DC00, false}, {0x0000DE00, true},  {0x0000E000, false}, {0x0000E200, false},
        {0x0000E400, false}, {0x0000E600, false}, {0x0000E800, false}, {0x0000EA00, false}, {0x0000EC00, false},
        {0x0000EE00, true},  {0x0000F000, false}, {0x0000F200, false}, {0x0000F400, false}, {0x0000F600, false},
        {0x0000F800, false}, {0x0000FA00, false}, {0x0000FC00, false}, {0x0000FE00, true},
    }};
    constexpr std::array<std::uint32_t, 8> kCommitStarts{0x00008000, 0x00009000, 0x0000A000, 0x0000B000,
                                                         0x0000C000, 0x0000D000, 0x0000E000, 0x0000F000};

    transport.ExpectIsoWrite(IsoRequest(0x04));
    transport.QueueIsoRead(IsoResponse(0x44, bytes::Bytes{0x00, 0x64}));
    transport.ExpectIsoWrite(IsoRequest(0x25, ComposeBe(std::uint32_t{0x00008000})));
    transport.QueueIsoRead(IsoResponse(0x65));
    const bytes::Bytes raw_chunk = FixedRawZeroBytes(kWriteChunkSize);
    std::size_t commit_index = 0;
    for (const FlashBufferExpectation& buffer : kBuffers)
    {
        transport.ExpectIsoWrite(IsoRequest(0x22, ComposeBe(buffer.address, raw_chunk)));
        transport.QueueIsoRead(IsoResponse(0x62));
        if (buffer.commit_after)
        {
            transport.ExpectIsoWrite(
                IsoRequest(0x24, ComposeBe(kCommitStarts[commit_index], std::uint16_t{kCommitBlockSize}, 0xF722EF49U)));
            transport.QueueIsoRead(IsoResponse(0x64));
            ++commit_index;
        }
    }
    EXPECT_EQ(commit_index, kCommitStarts.size());
}

bool HasLog(const RecordingEventSink& events, LogLevel level, std::string_view text)
{
    return std::any_of(events.logs.begin(), events.logs.end(), [level, text](const auto& log)
                       { return log.first == level && log.second.find(text) != std::string::npos; });
}

constexpr std::string_view kReflashRecoveryWarning =
    "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most likely still running and "
    "receiving commands!";

int ExactLogCount(const RecordingEventSink& events, LogLevel level, std::string_view text)
{
    return static_cast<int>(std::count_if(events.logs.begin(), events.logs.end(), [level, text](const auto& log)
                                          { return log.first == level && log.second == text; }));
}

struct ExpectedPhaseProgress
{
    std::string_view name;
    int index;
    int count;
    int done;
    int total;
};

std::vector<ExpectedPhaseProgress> ExpectedWritePhaseTrace(std::string_view write_phase)
{
    return {
        {"Kernel", 1, 4, 0, 1},          {"Kernel", 1, 4, 1, 1},          {"Compare", 2, 4, 0, 16},
        {"Compare", 2, 4, 1, 16},        {"Compare", 2, 4, 2, 16},        {"Compare", 2, 4, 3, 16},
        {"Compare", 2, 4, 4, 16},        {"Compare", 2, 4, 5, 16},        {"Compare", 2, 4, 6, 16},
        {"Compare", 2, 4, 7, 16},        {"Compare", 2, 4, 8, 16},        {"Compare", 2, 4, 9, 16},
        {"Compare", 2, 4, 10, 16},       {"Compare", 2, 4, 11, 16},       {"Compare", 2, 4, 12, 16},
        {"Compare", 2, 4, 13, 16},       {"Compare", 2, 4, 14, 16},       {"Compare", 2, 4, 15, 16},
        {"Compare", 2, 4, 16, 16},       {write_phase, 3, 4, 0, 4096},    {write_phase, 3, 4, 512, 4096},
        {write_phase, 3, 4, 1024, 4096}, {write_phase, 3, 4, 1536, 4096}, {write_phase, 3, 4, 2048, 4096},
        {write_phase, 3, 4, 2560, 4096}, {write_phase, 3, 4, 3072, 4096}, {write_phase, 3, 4, 3584, 4096},
        {write_phase, 3, 4, 4095, 4096}, {write_phase, 3, 4, 4096, 4096}, {"Complete", 4, 4, 0, 1},
        {"Complete", 4, 4, 1, 1},
    };
}

std::vector<ExpectedPhaseProgress> ExpectedUnchangedWritePhaseTrace(std::string_view write_phase)
{
    return {
        {"Kernel", 1, 4, 0, 1},    {"Kernel", 1, 4, 1, 1},    {"Compare", 2, 4, 0, 16},  {"Compare", 2, 4, 1, 16},
        {"Compare", 2, 4, 2, 16},  {"Compare", 2, 4, 3, 16},  {"Compare", 2, 4, 4, 16},  {"Compare", 2, 4, 5, 16},
        {"Compare", 2, 4, 6, 16},  {"Compare", 2, 4, 7, 16},  {"Compare", 2, 4, 8, 16},  {"Compare", 2, 4, 9, 16},
        {"Compare", 2, 4, 10, 16}, {"Compare", 2, 4, 11, 16}, {"Compare", 2, 4, 12, 16}, {"Compare", 2, 4, 13, 16},
        {"Compare", 2, 4, 14, 16}, {"Compare", 2, 4, 15, 16}, {"Compare", 2, 4, 16, 16}, {write_phase, 3, 4, 0, 0},
        {"Complete", 4, 4, 0, 1},  {"Complete", 4, 4, 1, 1},
    };
}

void ExpectPhaseTrace(const RecordingEventSink& events, std::string_view write_phase)
{
    const std::vector<ExpectedPhaseProgress> expected = ExpectedWritePhaseTrace(write_phase);
    ASSERT_EQ(events.phase_progress_calls.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        const auto& actual = events.phase_progress_calls[index];
        const auto& want = expected[index];
        EXPECT_EQ(actual.phase_name, want.name) << index;
        EXPECT_EQ(actual.phase_index, want.index) << index;
        EXPECT_EQ(actual.phase_count, want.count) << index;
        EXPECT_EQ(actual.done, want.done) << index;
        EXPECT_EQ(actual.total, want.total) << index;
    }
}

void ExpectUnchangedWritePhaseTrace(const RecordingEventSink& events, std::string_view write_phase)
{
    const std::vector<ExpectedPhaseProgress> expected = ExpectedUnchangedWritePhaseTrace(write_phase);
    ASSERT_EQ(events.phase_progress_calls.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        const auto& actual = events.phase_progress_calls[index];
        const auto& want = expected[index];
        EXPECT_EQ(actual.phase_name, want.name) << index;
        EXPECT_EQ(actual.phase_index, want.index) << index;
        EXPECT_EQ(actual.phase_count, want.count) << index;
        EXPECT_EQ(actual.done, want.done) << index;
        EXPECT_EQ(actual.total, want.total) << index;
    }
}

void ExpectTwoBlockWriteProgress(const RecordingEventSink& events)
{
    constexpr std::array<int, 74> kExpectedDone{
        0,     512,   1024,  1536,  2048,  2560,  3072,  3584,  4096,  4608,  5120,  5632,  6144,  6656,  7168,
        7680,  8192,  8704,  9216,  9728,  10240, 10752, 11264, 11776, 12288, 12800, 13312, 13824, 14336, 14848,
        15360, 15872, 16384, 16896, 17408, 17920, 18432, 18944, 19456, 19968, 20480, 20992, 21504, 22016, 22528,
        23040, 23552, 24064, 24576, 25088, 25600, 26112, 26624, 27136, 27648, 28160, 28672, 29184, 29696, 30208,
        30720, 31232, 31744, 32256, 32768, 33280, 33792, 34304, 34816, 35328, 35840, 36352, 36863, 36864,
    };
    std::vector<RecordedPhaseProgress> write_updates;
    for (const auto& update : events.phase_progress_calls)
    {
        if (update.phase_name == "Write")
        {
            write_updates.push_back(update);
        }
    }
    ASSERT_EQ(write_updates.size(), kExpectedDone.size());
    for (std::size_t index = 0; index < kExpectedDone.size(); ++index)
    {
        EXPECT_EQ(write_updates[index].phase_index, 3) << index;
        EXPECT_EQ(write_updates[index].phase_count, 4) << index;
        EXPECT_EQ(write_updates[index].done, kExpectedDone[index]) << index;
        EXPECT_EQ(write_updates[index].total, 0x9000) << index;
    }
}

using LogRecord = std::pair<LogLevel, std::string>;

void AppendLegacyCompareLogs(std::vector<LogRecord>& logs, const FlashDevice& device, bool first_block_mismatches,
                             bool after_reflash)
{
    logs.emplace_back(LogLevel::kInfo, after_reflash
                                           ? "--- Comparing ECU flash memory pages to image file after reflash ---"
                                           : "--- Comparing ECU flash memory pages to image file ---");
    logs.emplace_back(LogLevel::kInfo, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion block{device.fblocks[index].start, device.fblocks[index].len};
        const bool differs = first_block_mismatches && index == 0;
        const std::uint32_t image_crc = FixedRawZeroCrc(block, false);
        const std::uint32_t ecu_crc = FixedRawZeroCrc(block, differs);
        logs.emplace_back(LogLevel::kInfo,
                          std::format("FB{:02}\t0x{:08X}\t0x{:08X}", index, block.start, block.length));
        logs.emplace_back(LogLevel::kDebug, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", ecu_crc, image_crc));
        logs.emplace_back(LogLevel::kInfo, std::format("\t{:08X}\t{:08X}", ecu_crc, image_crc));
        logs.emplace_back(LogLevel::kInfo, differs ? "\tNO" : "\tYES");
    }
    logs.emplace_back(LogLevel::kInfo, "Different blocks : ");
    if (first_block_mismatches)
    {
        logs.emplace_back(LogLevel::kInfo, "0, ");
    }
    logs.emplace_back(LogLevel::kInfo, first_block_mismatches ? " (total: 1)" : " (total: 0)");
}

std::vector<LogRecord> ExpectedLegacyWriteLogs(const FlashDevice& device, FlashOperation operation)
{
    const bool test_write = operation == FlashOperation::kTestWrite;
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::kInfo, "Checking if Kernel already running...");
    logs.emplace_back(LogLevel::kInfo, "Requesting kernel ID");
    logs.emplace_back(LogLevel::kInfo, "Kernel ID: KID");
    AppendLegacyCompareLogs(logs, device, true, false);
    logs.emplace_back(LogLevel::kInfo, "--- Start writing ROM file to ECU flash memory ---");
    logs.emplace_back(LogLevel::kInfo, "Check max message length");
    logs.emplace_back(LogLevel::kInfo, ": 0x0200");
    logs.emplace_back(LogLevel::kInfo, "Check flashblock size");
    logs.emplace_back(LogLevel::kInfo, ": 0x1000");
    logs.emplace_back(LogLevel::kInfo, test_write ? "Test write mode on, no actual flash write is performed"
                                                  : "Test write mode off, perform actual flash write");
    logs.emplace_back(LogLevel::kError, "Flash mode succesfully set");
    logs.emplace_back(LogLevel::kInfo, "Flash block addr: 0x00000000 len: 0x00001000");
    logs.emplace_back(LogLevel::kInfo, "Check flash voltage");
    logs.emplace_back(LogLevel::kInfo, ": 2V");
    if (!test_write)
    {
        logs.emplace_back(LogLevel::kInfo, "Flash page erase addr: 0x00000000 len: 0x00001000");
        logs.emplace_back(LogLevel::kInfo, "Erasing flash page...");
        logs.emplace_back(LogLevel::kInfo, " erased");
    }
    logs.emplace_back(LogLevel::kInfo, "Start flash write addr: 0x00000000 len: 0x00001000");
    constexpr std::array<std::uint32_t, 8> kBufferAddresses{0x00000000, 0x00000200, 0x00000400, 0x00000600,
                                                            0x00000800, 0x00000A00, 0x00000C00, 0x00000E00};
    constexpr std::array<unsigned, 8> kPercents{0, 12, 25, 37, 50, 62, 75, 87};
    for (std::size_t index = 0; index < kBufferAddresses.size(); ++index)
    {
        logs.emplace_back(LogLevel::kDebug, "Data written to flash buffer");
        logs.emplace_back(LogLevel::kInfo, std::format("Write flash buffer: 0x{:08X} ({}% - 512000 B/s, ~ 1 s)",
                                                       kBufferAddresses[index], kPercents[index]));
    }
    logs.emplace_back(LogLevel::kInfo, "Flash buffer write complete... ");
    logs.emplace_back(LogLevel::kDebug, "Image CRC32: 0xf722ef49");
    logs.emplace_back(LogLevel::kInfo, test_write ? "Validate flash addr: 0x0" : "Committ flash addr: 0x0");
    logs.emplace_back(LogLevel::kInfo, " len: 0x1000");
    logs.emplace_back(LogLevel::kInfo, " crc32: 0xf722ef49");
    logs.emplace_back(LogLevel::kInfo, "Flash block ok");
    logs.emplace_back(LogLevel::kInfo, "Block 0 reflash complete.");
    AppendLegacyCompareLogs(logs, device, false, true);
    if (test_write)
    {
        logs.emplace_back(LogLevel::kInfo, "*** Test write PASS, it's ok to perform actual write! ***");
    }
    return logs;
}

std::vector<LogRecord> ExpectedLegacyUnchangedWriteLogs(const FlashDevice& device)
{
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::kInfo, "Checking if Kernel already running...");
    logs.emplace_back(LogLevel::kInfo, "Requesting kernel ID");
    logs.emplace_back(LogLevel::kInfo, "Kernel ID: KID");
    AppendLegacyCompareLogs(logs, device, false, false);
    logs.emplace_back(LogLevel::kInfo,
                      "*** Compare results no difference between ROM and ECU data, no flashing needed! ***");
    return logs;
}

std::vector<LogRecord> ExpectedLegacyReadLogs(std::uint32_t rom_size)
{
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::kInfo, "Checking if Kernel already running...");
    logs.emplace_back(LogLevel::kInfo, "Requesting kernel ID");
    logs.emplace_back(LogLevel::kInfo, "Kernel ID: KID");
    logs.emplace_back(LogLevel::kInfo, "Start reading ROM, please wait...");
    for (std::uint32_t address = 0; address < rom_size; address += kReadPageSize)
    {
        // The controller-approved correction clamps FakeClock's zero elapsed
        // interval to one millisecond before applying legacy's speed formula.
        logs.emplace_back(
            LogLevel::kInfo,
            std::format("Kernel read addr: 0x{:08X} length: 0x00000400, 1024000 B/s {:>6} s", address, 1));
    }
    logs.emplace_back(LogLevel::kInfo, "ROM read ready");
    return logs;
}

std::vector<LogRecord> ExpectedLegacyUploadReadLogs(std::uint32_t rom_size)
{
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::kInfo, "Checking if Kernel already running...");
    logs.emplace_back(LogLevel::kInfo, "Requesting kernel ID");
    logs.emplace_back(LogLevel::kError, "No valid response from ECU");
    logs.emplace_back(LogLevel::kInfo, "No response from kernel, continue initializing bootloader...");
    logs.emplace_back(LogLevel::kInfo, "Initializing bootloader");
    logs.emplace_back(LogLevel::kInfo, "Check if connected to bootloader");
    logs.emplace_back(LogLevel::kDebug, "Connected to bootloader");
    logs.emplace_back(LogLevel::kDebug, "Start address to upload kernel: ffff6004");
    logs.emplace_back(LogLevel::kInfo, "Set kernel upload address");
    logs.emplace_back(LogLevel::kDebug, "Kernel load address set");
    logs.emplace_back(LogLevel::kInfo, "Uploading kernel, please wait...");
    logs.emplace_back(LogLevel::kDebug, "Sending 1 blocks");
    logs.emplace_back(LogLevel::kDebug, "All kernel blocks sent, checksum: 0xff");
    logs.emplace_back(LogLevel::kDebug, "Verifying kernel checksum, please wait...");
    logs.emplace_back(LogLevel::kDebug, "Checksum ok");
    logs.emplace_back(LogLevel::kInfo, "Kernel uploaded, jump to kernel");
    logs.emplace_back(LogLevel::kError, "No valid response from ECU");
    logs.emplace_back(LogLevel::kInfo, "Requesting kernel ID");
    logs.emplace_back(LogLevel::kInfo, "Kernel ID: KID");
    logs.emplace_back(LogLevel::kInfo, "Start reading ROM, please wait...");
    for (std::uint32_t address = 0; address < rom_size; address += kReadPageSize)
    {
        logs.emplace_back(
            LogLevel::kInfo,
            std::format("Kernel read addr: 0x{:08X} length: 0x00000400, 1024000 B/s {:>6} s", address, 1));
    }
    logs.emplace_back(LogLevel::kInfo, "ROM read ready");
    return logs;
}

// Kernel ID request literal, derived from legacy request_kernel_id() lines
// 1435-1484: [000007e0][beef][0001][01][000000]. This locks the helper above
// independently of the executor's private framing logic.
TEST(SubaruDensoSh705xDensoCanExecutor, KernelIdFrameMatchesHandDerivedWireBytes)
{
    EXPECT_THAT(KernelIdRequest(), ElementsAre(0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00));
    EXPECT_THAT(IsoRequest(0x03, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x04, 0x00}),
                ElementsAre(0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00));
}

TEST(SubaruDensoSh705xDensoCanExecutor, TransportSetupUsesExactMixedCanConfigurationForEveryCatalogEntry)
{
    SubaruDensoSh705xDensoCanExecutor executor;
    for (const Case& test_case : kCases)
    {
        auto plan = ReadPlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto setup = executor.TransportSetup(*plan);
        ASSERT_TRUE(setup.has_value()) << setup.error().detail;
        EXPECT_EQ(setup->kernel.bitrate, 500000);
        EXPECT_EQ(setup->kernel.request_id, kIsoRequestId);
        EXPECT_EQ(setup->kernel.response_id, kIsoResponseId);
        EXPECT_FALSE(setup->kernel.extended_id);
        EXPECT_EQ(setup->bootloader.bitrate, 500000);
        EXPECT_EQ(setup->bootloader.transmit_id, kRawTransmitId);
        EXPECT_EQ(setup->bootloader.receive_id, kRawReceiveId);
        EXPECT_TRUE(setup->bootloader.extended_id);
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, BoundAttemptResetsBeforeMixedConfigurationAndOpen)
{
    auto plan = ReadPlan(kCases.front());
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto transport = std::make_unique<TimeoutRecordingMixedCanTransport>();
    TimeoutRecordingMixedCanTransport *observed = transport.get();
    FakeCancellationToken cancellation;
    observed->cancellation_on_open = &cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto attempt =
        BindFlashAttempt(std::move(*plan), std::make_unique<SubaruDensoSh705xDensoCanExecutor>(), std::move(transport));
    const auto result = attempt->Run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
    EXPECT_THAT(observed->lifecycle, ElementsAre("reset_connection", "configure", "open", "close"));
}

TEST(SubaruDensoSh705xDensoCanExecutor, StartupCancellationBeforeResetTouchesNoLifecycleOperation)
{
    auto plan = ReadPlan(kCases.front());
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto transport = std::make_unique<TimeoutRecordingMixedCanTransport>();
    TimeoutRecordingMixedCanTransport *observed = transport.get();
    FakeCancellationToken cancellation;
    cancellation.SetCancelled(true);
    FakeClock clock;
    RecordingEventSink events;

    auto attempt =
        BindFlashAttempt(std::move(*plan), std::make_unique<SubaruDensoSh705xDensoCanExecutor>(), std::move(transport));
    const auto result = attempt->Run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
    EXPECT_TRUE(observed->lifecycle.empty());
}

TEST(SubaruDensoSh705xDensoCanExecutor, StartupCancellationAfterResetSkipsConfigureOpenAndClose)
{
    auto plan = ReadPlan(kCases.front());
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto transport = std::make_unique<TimeoutRecordingMixedCanTransport>();
    TimeoutRecordingMixedCanTransport *observed = transport.get();
    FakeCancellationToken cancellation;
    observed->cancellation_on_reset = &cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto attempt =
        BindFlashAttempt(std::move(*plan), std::make_unique<SubaruDensoSh705xDensoCanExecutor>(), std::move(transport));
    const auto result = attempt->Run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
    EXPECT_THAT(observed->lifecycle, ElementsAre("reset_connection"));
}

TEST(SubaruDensoSh705xDensoCanExecutor, StartupResetFailurePropagatesWithoutConfigureOpenOrClose)
{
    auto plan = ReadPlan(kCases.front());
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto transport = std::make_unique<TimeoutRecordingMixedCanTransport>();
    TimeoutRecordingMixedCanTransport *observed = transport.get();
    observed->reset_result = Fail(ErrorKind::kInternal, "mixed reset marker");
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto attempt =
        BindFlashAttempt(std::move(*plan), std::make_unique<SubaruDensoSh705xDensoCanExecutor>(), std::move(transport));
    const auto result = attempt->Run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kInternal);
    EXPECT_EQ(result.error().detail, "mixed reset marker");
    EXPECT_THAT(observed->lifecycle, ElementsAre("reset_connection"));
}

TEST(SubaruDensoSh705xDensoCanExecutor, AlreadyRunningKernelReadsEveryPageAndReturnsTheRawRom)
{
    const Case& test_case = kCases.front();
    auto plan = ReadPlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptLiveRead(transport, test_case, 0x00);
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), test_case.rom_size);
    // revision-59f4e442 read_mem() appends the BEEF page payload untouched.
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(4), ElementsAre(0x00, 0x00, 0x00, 0x00));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.ModeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::kIso15765Kernel}));
    EXPECT_EQ(events.logs, ExpectedLegacyReadLogs(test_case.rom_size));
    EXPECT_FALSE(events.progress_calls.empty());
    ASSERT_FALSE(events.phase_progress_calls.empty());
    EXPECT_EQ(events.phase_progress_calls.front().phase_name, "Kernel");
    EXPECT_EQ(events.phase_progress_calls.front().phase_count, 2);
    EXPECT_EQ(events.phase_progress_calls.back().phase_name, "Read");
    EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(test_case.rom_size));
}

TEST(SubaruDensoSh705xDensoCanExecutor, ReadKeepsRawBeefBoundaryPageBytesForRepresentativeGeometry)
{
    const Case& test_case = kCases.front();
    auto plan = ReadPlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAlive(transport);
    ScriptRawReadPagesWithBoundarySentinels(transport, test_case.rom_size);
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(8),
                ElementsAre(0xD3, 0x5A, 0xC7, 0x19, 0x2E, 0xF4, 0x80, 0x6B));
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).last(8),
                ElementsAre(0x9C, 0x31, 0xE7, 0x04, 0xB2, 0x6D, 0x58, 0xAF));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, WriteAndTestWriteUseCallerRawImageBytesAndCommitCrc)
{
    const Case& test_case = kCases.front();
    const FlashDevice *device = FindFlashDevice(test_case.mcu);
    ASSERT_NE(device, nullptr);
    for (const FlashOperation operation : {FlashOperation::kWrite, FlashOperation::kTestWrite})
    {
        SCOPED_TRACE(operation == FlashOperation::kWrite ? "Write" : "TestWrite");
        auto plan = WritePlan(test_case, operation, CallerRawDensocanImage(test_case));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptRawImageCompare(transport, *device, false);
        ScriptFlashInit(transport, operation == FlashOperation::kTestWrite);
        ScriptRawFirstFlashBlock(transport, operation == FlashOperation::kTestWrite);
        ScriptRawImageCompare(transport, *device, true);
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << result.error().detail;
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, ProbeTimeoutTransitionsThroughRawUploadAndBackBeforeReading)
{
    const Case& test_case = kCases.front();
    auto plan = ReadPlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    transport.ExpectIsoWrite(KernelIdRequest());
    transport.QueueNoIsoFrame();
    ScriptUpload(transport);
    transport.ExpectIsoWrite(KernelIdRequest());
    transport.QueueIsoRead(KernelIdResponse());
    ScriptReadPages(transport, test_case.rom_size);
    FakeCancellationToken cancellation;
    MockClock clock;
    EXPECT_CALL(clock, Sleep(3ms, _)).Times(AtLeast(1));
    EXPECT_CALL(clock, Sleep(1ms, _)).Times(AtLeast(1));
    EXPECT_CALL(clock, Sleep(200ms, _)).Times(AtLeast(1));
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.clear_receive_buffer_call_count, 1);
    EXPECT_EQ(transport.ModeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::kIso15765Kernel,
                                                                          ScriptedMixedCanMode::kRawBootloader,
                                                                          ScriptedMixedCanMode::kIso15765Kernel}));
    EXPECT_EQ(events.logs, ExpectedLegacyUploadReadLogs(test_case.rom_size));
}

TEST(SubaruDensoSh705xDensoCanExecutor, UploadPaddingAndChecksumUseHandDerivedSevenByteWireLiterals)
{
    const Case& test_case = kCases.front();
    auto plan = ReadPlan(test_case, bytes::Bytes{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77});
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    transport.ExpectIsoWrite(KernelIdRequest());
    transport.QueueNoIsoFrame();
    for (int count = 0; count < 1000; ++count)
    {
        transport.ExpectRawWrite(RawRequest({0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    }
    transport.ExpectRawWrite(RawRequest({0x7A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.QueueRawRead(RawResponse({0x7A, 0x96, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.ExpectRawWrite(RawRequest({0x7A, 0x9C, 0xFF, 0xFF, 0x60, 0x04, 0x00, 0x00}));
    transport.QueueRawRead(RawResponse({0x7A, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    // Hand-derived r59f4e442 upload literals: seven data bytes become two
    // six-byte blocks and the checksum address is FFFF6004 + 0x0C + 1.
    transport.ExpectRawWrite(RawRequest({0x7A, 0xAE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}));
    transport.ExpectRawWrite(RawRequest({0x7A, 0xAE, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.ExpectRawWrite(RawRequest({0x7A, 0xB4, 0xFF, 0xFF, 0x60, 0x11, 0x00, 0x00}));
    transport.QueueRawRead(RawResponse({0x7A, 0xB1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.ExpectRawWrite(RawRequest({0x7A, 0x9C, 0xFF, 0xFF, 0x60, 0x04, 0x00, 0x00}));
    transport.QueueRawRead(RawResponse({0x7A, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.ExpectRawWrite(RawRequest({0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.QueueNoRawFrame();
    transport.ExpectIsoWrite(KernelIdRequest());
    transport.QueueIsoRead(KernelIdResponse());
    transport.ExpectIsoWrite(IsoRequest(0x03, ComposeBe(0x00_b, U24(0), std::uint16_t{kReadPageSize})));
    transport.QueueIsoRead(IsoResponse(0x43, bytes::Bytes{0x00}));
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, NonzeroLargeBlockUsesEightFixedCommitWindowsAndCumulativeWriteProgress)
{
    const Case& test_case = kCases.front();
    const FlashDevice *device = FindFlashDevice(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = WritePlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAlive(transport);
    ScriptCompareWithModifiedBlocks(transport, *device, {0, 8});
    ScriptFlashInit(transport, false);
    ScriptFirstFlashBlock(transport, false);
    ScriptLargeNonzeroWriteBlock(transport);
    ScriptCompare(transport, *device, false);
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.ScriptConsumed());
    ExpectTwoBlockWriteProgress(events);
}

TEST(SubaruDensoSh705xDensoCanExecutor, KernelIdProbeAndPostUploadVerificationWaitForTheLegacy200Milliseconds)
{
    const Case& test_case = kCases.front();
    auto plan = ReadPlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    TimeoutRecordingMixedCanTransport transport;
    ConfigureAndOpen(executor, *plan, transport.scripted);
    transport.scripted.ExpectIsoWrite(KernelIdRequest());
    transport.scripted.QueueNoIsoFrame();
    ScriptUpload(transport.scripted);
    transport.scripted.ExpectIsoWrite(KernelIdRequest());
    transport.scripted.QueueIsoRead(KernelIdResponse());
    ScriptReadPages(transport.scripted, test_case.rom_size);
    FakeCancellationToken cancellation;
    MockClock clock;
    EXPECT_CALL(clock, Sleep(3ms, _)).Times(1000);
    EXPECT_CALL(clock, Sleep(1ms, _)).Times(1);
    // upload_kernel() already owns two 200 ms waits (checksum and jump).
    // request_kernel_id() adds one before the initial probe and one before
    // the post-upload kernel-ID verification.
    EXPECT_CALL(clock, Sleep(200ms, _)).Times(4);
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scripted.ScriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, CrcIntervalsAndFlashBufferAcknowledgementsUseTheLegacyTiming)
{
    const Case& test_case = kCases.front();
    const FlashDevice *device = FindFlashDevice(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = WritePlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    TimeoutRecordingMixedCanTransport transport;
    ConfigureAndOpen(executor, *plan, transport.scripted);
    ScriptKernelAlive(transport.scripted);
    ScriptCompare(transport.scripted, *device, true);
    ScriptFlashInit(transport.scripted, false);
    ScriptFirstFlashBlock(transport.scripted, false);
    ScriptCompare(transport.scripted, *device, false);
    FakeCancellationToken cancellation;
    MockClock clock;
    // get_changed_blocks() sleeps after every one of the sixteen CRC checks,
    // including the final check in each of the two comparison passes.
    EXPECT_CALL(clock, Sleep(5ms, _)).Times(32);
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(std::any_of(transport.iso_read_timeouts.begin(), transport.iso_read_timeouts.end(),
                            [](const auto& entry)
                            { return entry.first == std::optional<std::uint8_t>{0x22} && entry.second == 800ms; }));
    EXPECT_TRUE(transport.scripted.ScriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, AllCatalogGeometriesUseFullPagedReads)
{
    SubaruDensoSh705xDensoCanExecutor executor;
    FakeCancellationToken cancellation;
    FakeClock clock;
    for (const Case& test_case : kCases)
    {
        auto plan = ReadPlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptLiveRead(transport, test_case);
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << test_case.protocol << ": " << result.error().detail;
        ASSERT_TRUE(result->read_bytes.has_value());
        EXPECT_EQ(result->read_bytes->size(), test_case.rom_size);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, InitialKernelProbeTreatsLegacyNonterminalRepliesAsKernelAbsent)
{
    struct ProbeCase
    {
        std::string_view name;
        std::optional<bytes::Bytes> frame;
        std::optional<ErrorKind> error;
    };
    const std::array<ProbeCase, 7> cases{{
        {"no-frame", std::nullopt, std::nullopt},
        {"timeout", std::nullopt, ErrorKind::kTimeout},
        {"adapter-internal", std::nullopt, ErrorKind::kInternal},
        {"short", bytes::Bytes{0x00, 0x00, 0x07}, std::nullopt},
        {"malformed-length", bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE, 0xEF, 0x00, 0x02, 0x41}, std::nullopt},
        {"wrong-id", bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x01, 0x41}, std::nullopt},
        {"wrong-content", bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE, 0xEF, 0x00, 0x01, 0x42}, std::nullopt},
    }};

    for (const ProbeCase& probe : cases)
    {
        SCOPED_TRACE(probe.name);
        auto plan = ReadPlan(kCases.front());
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        transport.ExpectIsoWrite(KernelIdRequest());
        if (probe.error.has_value())
        {
            transport.QueueIsoError(*probe.error, "legacy initial probe read outcome");
        }
        else if (probe.frame.has_value())
        {
            transport.QueueIsoRead(*probe.frame);
        }
        else
        {
            transport.QueueNoIsoFrame();
        }
        // One literal wake frame proves fallback reached the next bootloader
        // exchange. Cancellation on its 3 ms pacing wait bounds each case.
        transport.ExpectRawWrite(RawRequest({0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        FakeCancellationToken cancellation;
        MockClock clock;
        EXPECT_CALL(clock, Sleep(3ms, _))
            .WillOnce(DoAll([&] { cancellation.SetCancelled(true); }, clock.SleepOnFake()));
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_EQ(transport.ModeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::kIso15765Kernel,
                                                                              ScriptedMixedCanMode::kRawBootloader}));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, InitialKernelProbeKeepsCancellationAndDisconnectTerminal)
{
    for (const ErrorKind terminal : {ErrorKind::kCancelled, ErrorKind::kDisconnected})
    {
        SCOPED_TRACE(static_cast<int>(terminal));
        auto plan = ReadPlan(kCases.front());
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        transport.ExpectIsoWrite(KernelIdRequest());
        transport.QueueIsoError(terminal, "terminal initial probe outcome");
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, terminal);
        EXPECT_EQ(transport.ModeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::kIso15765Kernel}));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, PostUploadKernelProbeRemainsStrict)
{
    auto plan = ReadPlan(kCases.front());
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    transport.ExpectIsoWrite(KernelIdRequest());
    transport.QueueNoIsoFrame();
    ScriptUpload(transport);
    transport.ExpectIsoWrite(KernelIdRequest());
    transport.QueueIsoRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE, 0xEF, 0x00, 0x02, 0x41});
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, RejectsBEEFResponseWhoseDeclaredLengthDoesNotCoverReadPayload)
{
    auto plan = ReadPlan(kCases.front());
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAlive(transport);
    transport.ExpectIsoWrite(IsoRequest(0x03, ComposeBe(0x00_b, U24(0), std::uint16_t{kReadPageSize})));
    bytes::Bytes truncated_declaration = IsoResponse(0x43, bytes::Bytes(kReadPageSize, 0x00));
    truncated_declaration[6] = 0x00;
    truncated_declaration[7] = 0x01;
    transport.QueueIsoRead(std::move(truncated_declaration));
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, RawTransitionAndRawResponseFailuresAreFailClosed)
{
    auto plan = ReadPlan(kCases.front());
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    {
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        transport.ExpectIsoWrite(KernelIdRequest());
        transport.QueueNoIsoFrame();
        transport.FailNextRawTransition();
        auto result = executor.Execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kInternal);
        EXPECT_EQ(transport.ModeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::kIso15765Kernel}));
        EXPECT_TRUE(transport.ScriptConsumed());
    }

    {
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        transport.ExpectIsoWrite(KernelIdRequest());
        transport.QueueNoIsoFrame();
        for (int count = 0; count < 1000; ++count)
        {
            transport.ExpectRawWrite(RawRequest({0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        }
        transport.ExpectRawWrite(RawRequest({0x7A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        transport.QueueRawRead(RawResponse({0x7A, 0x96, 0, 0, 0, 0, 0, 0}, 0x22));
        auto result = executor.Execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, IsoTransitionFailureIsFailClosedAfterRawUpload)
{
    const Case& test_case = kCases.front();
    auto plan = ReadPlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    transport.ExpectIsoWrite(KernelIdRequest());
    transport.QueueNoIsoFrame();
    ScriptUpload(transport);
    transport.FailNextIsoTransition();
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kInternal);
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.ModeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::kIso15765Kernel,
                                                                          ScriptedMixedCanMode::kRawBootloader}));
}

TEST(SubaruDensoSh705xDensoCanExecutor, TimeoutAndDisconnectPropagateWithoutAnUnsafeFallback)
{
    const Case& test_case = kCases.front();
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    {
        auto plan = ReadPlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        transport.ExpectIsoWrite(IsoRequest(0x03, ComposeBe(0x00_b, U24(0), std::uint16_t{kReadPageSize})));
        transport.QueueNoIsoFrame();

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kTimeout);
        EXPECT_TRUE(transport.ScriptConsumed());
    }

    {
        auto plan = ReadPlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        transport.ExpectIsoWrite(KernelIdRequest());
        transport.QueueIsoError(ErrorKind::kDisconnected, "adapter dropped");

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kDisconnected);
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_EQ(transport.ModeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::kIso15765Kernel}));
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, MalformedJumpAcknowledgementIsToleratedBeforeTheIsoProbe)
{
    const Case& test_case = kCases.front();
    auto plan = ReadPlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    transport.ExpectIsoWrite(KernelIdRequest());
    transport.QueueNoIsoFrame();
    ScriptUpload(transport, RawResponse({0x7A, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    ScriptKernelAlive(transport);
    ScriptReadPages(transport, test_case.rom_size);
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(ExactLogCount(events, LogLevel::kError, "Wrong response from ECU"), 1);
}

TEST(SubaruDensoSh705xDensoCanExecutor, ShortReadAndCrcPayloadsAreRejectedBeforeDecoding)
{
    const Case& test_case = kCases.front();
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    {
        auto plan = ReadPlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        transport.ExpectIsoWrite(IsoRequest(0x03, ComposeBe(0x00_b, U24(0), std::uint16_t{kReadPageSize})));
        transport.QueueIsoRead(IsoResponse(0x43, bytes::Bytes(kReadPageSize - 1, 0x00)));

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
        EXPECT_TRUE(transport.ScriptConsumed());
    }

    {
        auto plan = WritePlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        const FlashDevice *device = FindFlashDevice(test_case.mcu);
        ASSERT_NE(device, nullptr);
        const MemoryRegion block{device->fblocks[0].start, device->fblocks[0].len};
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        transport.ExpectIsoWrite(IsoRequest(0x02, ComposeBe(block.start, 0x00_b, U24(block.length))));
        transport.QueueIsoRead(IsoResponse(0x42, bytes::Bytes{0x00, 0x00, 0x00}));

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, ShortInitializationAndVoltagePayloadsAreRejectedBeforeDecoding)
{
    const Case& test_case = kCases.front();
    const FlashDevice *device = FindFlashDevice(test_case.mcu);
    ASSERT_NE(device, nullptr);
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    {
        auto plan = WritePlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptCompare(transport, *device, true);
        transport.ExpectIsoWrite(IsoRequest(0x05));
        transport.QueueIsoRead(IsoResponse(0x45, bytes::Bytes{0x00, 0x00, 0x00}));

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
        EXPECT_TRUE(transport.ScriptConsumed());
    }

    {
        auto plan = WritePlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptCompare(transport, *device, true);
        ScriptFlashInit(transport, false);
        transport.ExpectIsoWrite(IsoRequest(0x04));
        transport.QueueIsoRead(IsoResponse(0x44, bytes::Bytes{0x00}));

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, ShortRawAndFlashAcknowledgementsAreRejectedBeforeUse)
{
    const Case& test_case = kCases.front();
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    {
        auto plan = ReadPlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        transport.ExpectIsoWrite(KernelIdRequest());
        transport.QueueNoIsoFrame();
        for (int count = 0; count < 1000; ++count)
        {
            transport.ExpectRawWrite(RawRequest({0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        }
        transport.ExpectRawWrite(RawRequest({0x7A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        transport.QueueRawRead(RawResponse({0x7A}));

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
        EXPECT_TRUE(transport.ScriptConsumed());
    }

    {
        auto plan = WritePlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        const FlashDevice *device = FindFlashDevice(test_case.mcu);
        ASSERT_NE(device, nullptr);
        const bytes::Bytes raw_chunk = FixedRawZeroBytes(kWriteChunkSize);
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptCompare(transport, *device, true);
        ScriptFlashInit(transport, false);
        transport.ExpectIsoWrite(IsoRequest(0x04));
        transport.QueueIsoRead(IsoResponse(0x44, bytes::Bytes{0x00, 0x64}));
        transport.ExpectIsoWrite(IsoRequest(0x25, ComposeBe(std::uint32_t{0})));
        transport.QueueIsoRead(IsoResponse(0x65));
        transport.ExpectIsoWrite(IsoRequest(0x22, ComposeBe(std::uint32_t{0}, raw_chunk)));
        transport.QueueIsoRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE, 0xEF, 0x00, 0x01});

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, PostEraseProtocolFailureEmitsTheLegacyRecoveryWarning)
{
    const Case& test_case = kCases.front();
    const FlashDevice *device = FindFlashDevice(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = WritePlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    const bytes::Bytes raw_chunk = FixedRawZeroBytes(kWriteChunkSize);
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAlive(transport);
    ScriptCompare(transport, *device, true);
    ScriptFlashInit(transport, false);
    transport.ExpectIsoWrite(IsoRequest(0x04));
    transport.QueueIsoRead(IsoResponse(0x44, bytes::Bytes{0x00, 0x64}));
    transport.ExpectIsoWrite(IsoRequest(0x25, ComposeBe(std::uint32_t{0})));
    transport.QueueIsoRead(IsoResponse(0x65));
    transport.ExpectIsoWrite(IsoRequest(0x22, ComposeBe(std::uint32_t{0}, raw_chunk)));
    transport.QueueIsoRead(IsoResponse(0x7F));
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_EQ(ExactLogCount(events, LogLevel::kError, kReflashRecoveryWarning), 1);
    ASSERT_FALSE(events.logs.empty());
    EXPECT_EQ(events.logs.back().first, LogLevel::kError);
    EXPECT_EQ(events.logs.back().second, kReflashRecoveryWarning);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, CancellationAfterEraseEmitsTheLegacyRecoveryWarning)
{
    const Case& test_case = kCases.front();
    const FlashDevice *device = FindFlashDevice(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = WritePlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    const bytes::Bytes raw_chunk = FixedRawZeroBytes(kWriteChunkSize);
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAlive(transport);
    ScriptCompare(transport, *device, true);
    ScriptFlashInit(transport, false);
    transport.ExpectIsoWrite(IsoRequest(0x04));
    transport.QueueIsoRead(IsoResponse(0x44, bytes::Bytes{0x00, 0x64}));
    transport.ExpectIsoWrite(IsoRequest(0x25, ComposeBe(std::uint32_t{0})));
    transport.QueueIsoRead(IsoResponse(0x65));
    transport.ExpectIsoWrite(IsoRequest(0x22, ComposeBe(std::uint32_t{0}, raw_chunk)));
    transport.QueueIsoRead(IsoResponse(0x62));
    FakeCancellationToken cancellation;
    FakeClock clock;
    CancellingEventSink events(cancellation);

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
    EXPECT_EQ(ExactLogCount(events, LogLevel::kError, kReflashRecoveryWarning), 1);
    ASSERT_FALSE(events.logs.empty());
    EXPECT_EQ(events.logs.back().first, LogLevel::kError);
    EXPECT_EQ(events.logs.back().second, kReflashRecoveryWarning);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, FailureBeforeEraseDoesNotEmitTheRecoveryWarning)
{
    const Case& test_case = kCases.front();
    const FlashDevice *device = FindFlashDevice(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = WritePlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAlive(transport);
    ScriptCompare(transport, *device, true);
    ScriptFlashInit(transport, false);
    transport.ExpectIsoWrite(IsoRequest(0x04));
    transport.QueueIsoRead(IsoResponse(0x7F));
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_EQ(ExactLogCount(events, LogLevel::kError, kReflashRecoveryWarning), 0);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, CancellationStopsWakeAndUploadAtTheirLoopBoundaries)
{
    const Case& test_case = kCases.front();
    auto plan = ReadPlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    {
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        transport.ExpectIsoWrite(KernelIdRequest());
        transport.QueueNoIsoFrame();
        transport.ExpectRawWrite(RawRequest({0xFF, 0x86, 0, 0, 0, 0, 0, 0}));
        FakeCancellationToken cancellation;
        MockClock clock;
        EXPECT_CALL(clock, Sleep(3ms, _))
            .WillOnce(DoAll([&] { cancellation.SetCancelled(true); }, clock.SleepOnFake()));
        RecordingEventSink events;
        auto result = executor.Execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
    }

    {
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        transport.ExpectIsoWrite(KernelIdRequest());
        transport.QueueNoIsoFrame();
        for (int count = 0; count < 1000; ++count)
        {
            transport.ExpectRawWrite(RawRequest({0xFF, 0x86, 0, 0, 0, 0, 0, 0}));
        }
        transport.ExpectRawWrite(RawRequest({0x7A, 0x90, 0, 0, 0, 0, 0, 0}));
        transport.QueueRawRead(RawResponse({0x7A, 0x96, 0, 0, 0, 0, 0, 0}));
        const std::uint32_t address = test_case.kernel_address;
        transport.ExpectRawWrite(
            RawRequest({0x7A, 0x9C, static_cast<bytes::Byte>(address >> 24U), static_cast<bytes::Byte>(address >> 16U),
                        static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address), 0, 0}));
        transport.QueueRawRead(RawResponse({0x7A, 0x9C, 0, 0, 0, 0, 0, 0}));
        transport.ExpectRawWrite(RawRequest({0x7A, 0xAE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00}));
        FakeCancellationToken cancellation;
        MockClock clock;
        EXPECT_CALL(clock, Sleep(1ms, _))
            .WillOnce(DoAll([&] { cancellation.SetCancelled(true); }, clock.SleepOnFake()));
        RecordingEventSink events;
        auto result = executor.Execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, ReadAndWriteCancellationStopBeforeASecondTransfer)
{
    const Case& test_case = kCases.front();
    {
        auto plan = ReadPlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        transport.ExpectIsoWrite(IsoRequest(0x03, ComposeBe(0x00_b, U24(0), std::uint16_t{kReadPageSize})));
        transport.QueueIsoRead(IsoResponse(0x43, bytes::Bytes(kReadPageSize, 0)));
        FakeCancellationToken cancellation;
        FakeClock clock;
        CancellingEventSink events(cancellation);
        auto result = executor.Execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_TRUE(transport.ScriptConsumed());
    }

    {
        auto plan = WritePlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        const FlashDevice *device = FindFlashDevice(test_case.mcu);
        ASSERT_NE(device, nullptr);
        const bytes::Bytes raw_chunk = FixedRawZeroBytes(kWriteChunkSize);
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptCompare(transport, *device, true);
        ScriptFlashInit(transport, false);
        transport.ExpectIsoWrite(IsoRequest(0x04));
        transport.QueueIsoRead(IsoResponse(0x44, bytes::Bytes{0x00, 0x64}));
        transport.ExpectIsoWrite(IsoRequest(0x25, ComposeBe(std::uint32_t{0})));
        transport.QueueIsoRead(IsoResponse(0x65));
        transport.ExpectIsoWrite(IsoRequest(0x22, ComposeBe(std::uint32_t{0}, raw_chunk)));
        transport.QueueIsoRead(IsoResponse(0x62));
        FakeCancellationToken cancellation;
        FakeClock clock;
        CancellingEventSink events(cancellation);

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

void ExpectLegacyWriteLogs(FlashOperation operation)
{
    const Case& test_case = kCases.front();
    const FlashDevice *device = FindFlashDevice(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = WritePlan(test_case, operation);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAlive(transport);
    ScriptCompare(transport, *device, true);
    ScriptFlashInit(transport, operation == FlashOperation::kTestWrite);
    ScriptFirstFlashBlock(transport, operation == FlashOperation::kTestWrite);
    ScriptCompare(transport, *device, false);
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(events.logs, ExpectedLegacyWriteLogs(*device, operation));
}

TEST(SubaruDensoSh705xDensoCanExecutor, TestWriteOperatorLogsMatchTheCompleteLegacyRecord)
{
    ExpectLegacyWriteLogs(FlashOperation::kTestWrite);
}

TEST(SubaruDensoSh705xDensoCanExecutor, WriteOperatorLogsMatchTheCompleteLegacyRecord)
{
    ExpectLegacyWriteLogs(FlashOperation::kWrite);
}

TEST(SubaruDensoSh705xDensoCanExecutor,
     UnchangedWriteAndTestWriteEmitTheirZeroByteThirdPhaseBeforeTheFourthCompletePhase)
{
    const Case& test_case = kCases.front();
    const FlashDevice *device = FindFlashDevice(test_case.mcu);
    ASSERT_NE(device, nullptr);
    ASSERT_EQ(device->numblocks, 16U);
    for (const FlashOperation operation : {FlashOperation::kWrite, FlashOperation::kTestWrite})
    {
        auto plan = WritePlan(test_case, operation);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptCompare(transport, *device, false);
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << result.error().detail;
        // The strict script contains only the kernel-ID and all-equal CRC
        // exchanges, so success plus consumption proves no init, erase, or
        // programming traffic was sent.
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_EQ(events.logs, ExpectedLegacyUnchangedWriteLogs(*device));
        ExpectUnchangedWritePhaseTrace(events, operation == FlashOperation::kTestWrite ? "TestWrite" : "Write");
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, WriteAndTestWriteUseCrcInitAndDistinctEraseCommitCommands)
{
    const Case& test_case = kCases.front();
    const FlashDevice *device = FindFlashDevice(test_case.mcu);
    ASSERT_NE(device, nullptr);
    for (const FlashOperation operation : {FlashOperation::kWrite, FlashOperation::kTestWrite})
    {
        auto plan = WritePlan(test_case, operation);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptCompare(transport, *device, true);
        ScriptFlashInit(transport, operation == FlashOperation::kTestWrite);
        ScriptFirstFlashBlock(transport, operation == FlashOperation::kTestWrite);
        ScriptCompare(transport, *device, false);
        // The 59f4e442 oracle returns after the final comparison. In
        // particular, real-write has no trailing FLASH_DISABLE (0x21)
        // exchange; this script intentionally ends at verification so an
        // unsupported exchange fails the attempt.
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << result.error().detail;
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_EQ(ExactLogCount(events, LogLevel::kError, kReflashRecoveryWarning), 0);
        ExpectPhaseTrace(events, operation == FlashOperation::kTestWrite ? "TestWrite" : "Write");
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, BoundedAttemptClosesOnceAndPreservesExecutionErrorOverCloseError)
{
    const Case& test_case = kCases.front();
    auto plan = ReadPlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto executor = std::make_unique<SubaruDensoSh705xDensoCanExecutor>();
    auto transport = std::make_unique<ScriptedMixedCanFlashTransport>();
    ScriptedMixedCanFlashTransport *raw_transport = transport.get();
    ScriptLiveRead(*raw_transport, test_case);
    raw_transport->close_result = Fail(ErrorKind::kInternal, "close failed");
    auto attempt = BindFlashAttempt(std::move(*plan), std::move(executor), std::move(transport));
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = attempt->Run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kInternal);
    EXPECT_EQ(raw_transport->CloseCallCount(), 1);

    auto failing_plan = ReadPlan(test_case);
    ASSERT_TRUE(failing_plan.has_value()) << failing_plan.error().detail;
    auto failing_executor = std::make_unique<SubaruDensoSh705xDensoCanExecutor>();
    auto failing_transport = std::make_unique<ScriptedMixedCanFlashTransport>();
    ScriptedMixedCanFlashTransport *raw_failing_transport = failing_transport.get();
    raw_failing_transport->ExpectIsoWrite(KernelIdRequest());
    raw_failing_transport->QueueNoIsoFrame();
    raw_failing_transport->FailNextRawTransition();
    raw_failing_transport->close_result = Fail(ErrorKind::kTimeout, "close timeout");
    auto failing_attempt =
        BindFlashAttempt(std::move(*failing_plan), std::move(failing_executor), std::move(failing_transport));

    auto execution_error = failing_attempt->Run(clock, cancellation, events);

    ASSERT_FALSE(execution_error.has_value());
    EXPECT_EQ(execution_error.error().kind, ErrorKind::kInternal);
    EXPECT_EQ(raw_failing_transport->CloseCallCount(), 1);
    EXPECT_TRUE(HasLog(events, LogLevel::kWarning, "close failed after execution error"));
}

} // namespace
} // namespace fastecu::flash
