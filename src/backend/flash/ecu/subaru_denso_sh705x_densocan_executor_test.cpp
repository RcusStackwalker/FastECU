#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor.h"

#include <algorithm>
#include <array>
#include <atomic>
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
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/testing/scripted_mixed_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

using ::testing::Contains;
using ::testing::ElementsAre;

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::u24;
using namespace bytes::literals;

constexpr std::uint32_t kIsoRequestId = 0x7E0;
constexpr std::uint32_t kIsoResponseId = 0x7E8;
constexpr std::uint32_t kRawTransmitId = 0x000FFFFE;
constexpr std::uint32_t kRawReceiveId = 0x21;
constexpr std::uint32_t kReadPageSize = 0x400;
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitBlockSize = 0x1000;

class NeverCancelled final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return false;
    }
};

class ToggleCancellation final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return cancelled_.load();
    }
    void cancel()
    {
        cancelled_.store(true);
    }

  private:
    std::atomic_bool cancelled_{false};
};

class CancellingClock final : public FakeClock
{
  public:
    CancellingClock(ToggleCancellation& cancellation, int trigger_ms)
        : cancellation_(cancellation), trigger_ms_(trigger_ms)
    {
    }

    Status sleep(int ms, const ICancellationToken& cancellation) override
    {
        calls.push_back(ms);
        if (ms == trigger_ms_)
        {
            cancellation_.cancel();
        }
        return FakeClock::sleep(ms, cancellation);
    }

    std::vector<int> calls;

  private:
    ToggleCancellation& cancellation_;
    int trigger_ms_;
};

class RecordingClock final : public FakeClock
{
  public:
    Status sleep(int ms, const ICancellationToken& cancellation) override
    {
        sleeps.push_back(ms);
        return FakeClock::sleep(ms, cancellation);
    }

    std::vector<int> sleeps;
};

// Keeps the existing strict scripted transport while recording the timeout
// used for each BEEF read. The executor owns all protocol behavior; this is
// only an observation seam for legacy timing assertions.
class TimeoutRecordingMixedCanTransport final : public IMixedCanFlashTransport
{
  public:
    Status configure(const MixedCanConfig& config) override
    {
        return scripted.configure(config);
    }
    Status open() override
    {
        return scripted.open();
    }
    Status close() override
    {
        return scripted.close();
    }
    Status enter_raw_bootloader_mode() override
    {
        return scripted.enter_raw_bootloader_mode();
    }
    Status clear_receive_buffer() override
    {
        return scripted.clear_receive_buffer();
    }
    Status enter_iso15765_kernel_mode() override
    {
        return scripted.enter_iso15765_kernel_mode();
    }
    Status write_iso15765(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        last_iso_opcode_ = data.size() > 8 ? std::optional<std::uint8_t>{data[8]} : std::nullopt;
        return scripted.write_iso15765(data, cancellation);
    }
    Result<std::optional<bytes::Bytes>> read_iso15765(int timeout_ms, const ICancellationToken& cancellation) override
    {
        iso_read_timeouts.emplace_back(last_iso_opcode_, timeout_ms);
        return scripted.read_iso15765(timeout_ms, cancellation);
    }
    Status write_raw(const cdbg::CanFrame& frame, const ICancellationToken& cancellation) override
    {
        return scripted.write_raw(frame, cancellation);
    }
    Result<std::optional<cdbg::CanFrame>> read_raw(int timeout_ms, const ICancellationToken& cancellation) override
    {
        return scripted.read_raw(timeout_ms, cancellation);
    }
    void request_unblock() noexcept override
    {
        scripted.request_unblock();
    }

    ScriptedMixedCanFlashTransport scripted;
    std::vector<std::pair<std::optional<std::uint8_t>, int>> iso_read_timeouts;

  private:
    std::optional<std::uint8_t> last_iso_opcode_;
};

class CancellingEventSink final : public RecordingEventSink
{
  public:
    explicit CancellingEventSink(ToggleCancellation& cancellation) : cancellation_(cancellation)
    {
    }

    void progress(int done, int total) override
    {
        RecordingEventSink::progress(done, total);
        if (done > 0)
        {
            cancellation_.cancel();
        }
    }

    void phase_progress(const PhaseProgressEvent& event) override
    {
        phase_progress_calls.push_back(
            {std::string(event.phase_name), event.phase_index, event.phase_count, event.done, event.total});
        if ((event.phase_name == "Write" || event.phase_name == "TestWrite") && event.done > 0)
        {
            cancellation_.cancel();
        }
    }

  private:
    ToggleCancellation& cancellation_;
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

KernelImage kernel_for(const Case& test_case, bytes::Bytes data = {0x11, 0x22, 0x33, 0x44, 0x55})
{
    return {.id = "densocan-kernel", .load_address = test_case.kernel_address, .bytes = std::move(data)};
}

Result<FlashPlan> read_plan(const Case& test_case, bytes::Bytes kernel = {0x11, 0x22, 0x33, 0x44, 0x55})
{
    return build_subaru_denso_sh705x_densocan_plan(FlashOperation::Read, test_case.protocol, test_case.mcu,
                                                   std::nullopt, kernel_for(test_case, std::move(kernel)));
}

Result<FlashPlan> write_plan(const Case& test_case, FlashOperation operation = FlashOperation::Write,
                             bytes::Bytes image = {})
{
    if (image.empty())
    {
        image.assign(test_case.rom_size, bytes::Byte{0});
    }
    return build_subaru_denso_sh705x_densocan_plan(operation, test_case.protocol, test_case.mcu, std::move(image),
                                                   kernel_for(test_case));
}

bytes::Bytes iso_request(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes message = composeBe(kIsoRequestId, std::uint16_t{0xBEEF},
                                     static_cast<std::uint16_t>(payload.size() + 1), bytes::Byte(opcode));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

bytes::Bytes kernel_id_request()
{
    // Legacy request_kernel_id(), lines 1435-1484. The declared BEEF length
    // is one opcode byte, followed by three fixed zero bytes on the CAN wire.
    return {0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
}

bytes::Bytes iso_response(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes message = composeBe(kIsoResponseId, std::uint16_t{0xBEEF},
                                     static_cast<std::uint16_t>(payload.size() + 1), bytes::Byte(opcode));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

bytes::Bytes kernel_id_response()
{
    return iso_response(0x41, bytes::Bytes{'K', 'I', 'D'});
}

cdbg::CanFrame raw_request(bytes::Bytes payload)
{
    return {.id = kRawTransmitId, .payload = std::move(payload)};
}

cdbg::CanFrame raw_response(bytes::Bytes payload, std::uint32_t id = kRawReceiveId)
{
    return {.id = id, .payload = std::move(payload)};
}

void configure_and_open(SubaruDensoSh705xDensoCanExecutor& executor, const FlashPlan& plan,
                        ScriptedMixedCanFlashTransport& transport)
{
    auto setup = executor.transport_setup(plan);
    ASSERT_TRUE(setup.has_value()) << setup.error().detail;
    ASSERT_TRUE(transport.configure(*setup).has_value());
    ASSERT_TRUE(transport.open().has_value());
}

void script_kernel_alive(ScriptedMixedCanFlashTransport& transport)
{
    transport.expectIsoWrite(kernel_id_request());
    transport.queueIsoRead(kernel_id_response());
}

void script_read_pages(ScriptedMixedCanFlashTransport& transport, std::uint32_t size, bytes::Byte wire_fill = 0)
{
    for (std::uint32_t address = 0; address < size; address += kReadPageSize)
    {
        transport.expectIsoWrite(iso_request(0x03, composeBe(0x00_b, u24(address), std::uint16_t{kReadPageSize})));
        transport.queueIsoRead(iso_response(0x43, bytes::Bytes(kReadPageSize, wire_fill)));
    }
}

void script_live_read(ScriptedMixedCanFlashTransport& transport, const Case& test_case, bytes::Byte wire_fill = 0)
{
    script_kernel_alive(transport);
    script_read_pages(transport, test_case.rom_size, wire_fill);
}

void script_upload(ScriptedMixedCanFlashTransport& transport,
                   std::optional<cdbg::CanFrame> jump_response = std::nullopt)
{
    for (int count = 0; count < 1000; ++count)
    {
        transport.expectRawWrite(raw_request({0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    }
    transport.expectRawWrite(raw_request({0x7A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.queueRawRead(raw_response({0x7A, 0x96, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    transport.expectRawWrite(raw_request({0x7A, 0x9C, 0xFF, 0xFF, 0x60, 0x04, 0x00, 0x00}));
    transport.queueRawRead(raw_response({0x7A, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    // Hand-derived from the five-byte test kernel in legacy upload_kernel()
    // (r59f4e442 lines 227-507): its final six-byte block is zero-padded
    // and B4 carries FFFF6004 + 6 + 1 = FFFF600B. Keep these as fixed wire
    // literals; no test helper recomputes the production padding arithmetic.
    transport.expectRawWrite(raw_request({0x7A, 0xAE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00}));
    transport.expectRawWrite(raw_request({0x7A, 0xB4, 0xFF, 0xFF, 0x60, 0x0B, 0x00, 0x00}));
    transport.queueRawRead(raw_response({0x7A, 0xB1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    transport.expectRawWrite(raw_request({0x7A, 0x9C, 0xFF, 0xFF, 0x60, 0x04, 0x00, 0x00}));
    transport.queueRawRead(raw_response({0x7A, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.expectRawWrite(raw_request({0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    if (jump_response.has_value())
    {
        transport.queueRawRead(std::move(*jump_response));
    }
    else
    {
        transport.queueNoRawFrame(); // Legacy jump acknowledgement is tolerated.
    }
}

bytes::Bytes fixed_encrypted_zero_bytes(std::size_t size)
{
    constexpr std::array<bytes::Byte, 4> kEncryptedZeroWord{0xE8, 0x91, 0xF5, 0x06};
    bytes::Bytes wire;
    wire.reserve(size);
    for (std::size_t offset = 0; offset < size; offset += kEncryptedZeroWord.size())
    {
        wire.insert(wire.end(), kEncryptedZeroWord.begin(), kEncryptedZeroWord.end());
    }
    return wire;
}

void script_crc(ScriptedMixedCanFlashTransport& transport, const MemoryRegion& block, std::uint32_t crc)
{
    transport.expectIsoWrite(iso_request(0x02, composeBe(block.start, 0x00_b, u24(block.length))));
    transport.queueIsoRead(iso_response(0x42, composeBe(crc)));
    // Legacy check_romcrc() discards a short response after either comparison result.
    transport.queueNoIsoFrame();
}

std::uint32_t fixed_encrypted_zero_crc(const MemoryRegion& block, bool mismatch)
{
    switch (block.length)
    {
    case 0x00001000:
        return mismatch ? 0x9CC3CBD5U : 0x9CC3CBD4U;
    case 0x00008000:
        return mismatch ? 0xBEDCD1E6U : 0xBEDCD1E7U;
    case 0x00010000:
        return mismatch ? 0xB3FB13C5U : 0xB3FB13C4U;
    default:
        ADD_FAILURE() << "missing hand-derived DensoCAN CRC fixture for block length " << block.length;
        return 0;
    }
}

void script_compare(ScriptedMixedCanFlashTransport& transport, const flashdev_t& device, bool first_block_mismatches)
{
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion block{device.fblocks[index].start, device.fblocks[index].len};
        script_crc(transport, block, fixed_encrypted_zero_crc(block, first_block_mismatches && index == 0));
    }
}

void script_compare_with_modified_blocks(ScriptedMixedCanFlashTransport& transport, const flashdev_t& device,
                                         std::initializer_list<unsigned> modified_blocks)
{
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion block{device.fblocks[index].start, device.fblocks[index].len};
        const bool differs = std::find(modified_blocks.begin(), modified_blocks.end(), index) != modified_blocks.end();
        script_crc(transport, block, fixed_encrypted_zero_crc(block, differs));
    }
}

void script_flash_init(ScriptedMixedCanFlashTransport& transport, bool test_write)
{
    transport.expectIsoWrite(iso_request(0x05));
    transport.queueIsoRead(iso_response(0x45, composeBe(std::uint32_t{0x00000200})));
    transport.expectIsoWrite(iso_request(0x06));
    transport.queueIsoRead(iso_response(0x46, composeBe(std::uint32_t{0x00001000})));
    transport.expectIsoWrite(iso_request(test_write ? 0x21 : 0x20));
    transport.queueIsoRead(iso_response(test_write ? 0x61 : 0x60));
}

void script_first_flash_block(ScriptedMixedCanFlashTransport& transport, bool test_write)
{
    const std::uint32_t start = 0;
    // Legacy reflash_block() asks the programming-voltage question before
    // passing the block into flash_block(), which contains the erase.
    transport.expectIsoWrite(iso_request(0x04));
    transport.queueIsoRead(iso_response(0x44, bytes::Bytes{0x00, 0x64}));
    if (!test_write)
    {
        transport.expectIsoWrite(iso_request(0x25, composeBe(start)));
        transport.queueIsoRead(iso_response(0x65));
    }

    const bytes::Bytes encrypted_chunk = fixed_encrypted_zero_bytes(kWriteChunkSize);
    for (std::uint32_t offset = 0; offset < kCommitBlockSize; offset += kWriteChunkSize)
    {
        transport.expectIsoWrite(iso_request(0x22, composeBe(offset, encrypted_chunk)));
        transport.queueIsoRead(iso_response(0x62));
    }
    transport.expectIsoWrite(
        iso_request(test_write ? 0x23 : 0x24, composeBe(start, std::uint16_t{kCommitBlockSize}, 0x9CC3CBD4U)));
    transport.queueIsoRead(iso_response(test_write ? 0x63 : 0x64));
}

void script_large_nonzero_write_block(ScriptedMixedCanFlashTransport& transport)
{
    struct FlashBufferExpectation
    {
        std::uint32_t address;
        bool commit_after;
    };
    // Fixed, hand-derived windows for SH7055 fblocks[8] (0x8000/0x8000).
    // The literal commit starts prove eight independent 0x1000 windows;
    // this fixture deliberately contains no payload encryption or CRC logic.
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

    transport.expectIsoWrite(iso_request(0x04));
    transport.queueIsoRead(iso_response(0x44, bytes::Bytes{0x00, 0x64}));
    transport.expectIsoWrite(iso_request(0x25, composeBe(std::uint32_t{0x00008000})));
    transport.queueIsoRead(iso_response(0x65));
    const bytes::Bytes encrypted_chunk = fixed_encrypted_zero_bytes(kWriteChunkSize);
    std::size_t commit_index = 0;
    for (const FlashBufferExpectation& buffer : kBuffers)
    {
        transport.expectIsoWrite(iso_request(0x22, composeBe(buffer.address, encrypted_chunk)));
        transport.queueIsoRead(iso_response(0x62));
        if (buffer.commit_after)
        {
            transport.expectIsoWrite(iso_request(
                0x24, composeBe(kCommitStarts[commit_index], std::uint16_t{kCommitBlockSize}, 0x9CC3CBD4U)));
            transport.queueIsoRead(iso_response(0x64));
            ++commit_index;
        }
    }
    EXPECT_EQ(commit_index, kCommitStarts.size());
}

bool has_log(const RecordingEventSink& events, LogLevel level, std::string_view text)
{
    return std::any_of(events.logs.begin(), events.logs.end(), [level, text](const auto& log)
                       { return log.first == level && log.second.find(text) != std::string::npos; });
}

constexpr std::string_view kReflashRecoveryWarning =
    "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most likely still running and "
    "receiving commands!";

int exact_log_count(const RecordingEventSink& events, LogLevel level, std::string_view text)
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

std::vector<ExpectedPhaseProgress> expected_write_phase_trace(std::string_view write_phase)
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

void expect_phase_trace(const RecordingEventSink& events, std::string_view write_phase)
{
    const std::vector<ExpectedPhaseProgress> expected = expected_write_phase_trace(write_phase);
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

void expect_two_block_write_progress(const RecordingEventSink& events)
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

void append_legacy_compare_logs(std::vector<LogRecord>& logs, const flashdev_t& device, bool first_block_mismatches,
                                bool after_reflash)
{
    logs.emplace_back(LogLevel::Info, after_reflash
                                          ? "--- Comparing ECU flash memory pages to image file after reflash ---"
                                          : "--- Comparing ECU flash memory pages to image file ---");
    logs.emplace_back(LogLevel::Info, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion block{device.fblocks[index].start, device.fblocks[index].len};
        const bool differs = first_block_mismatches && index == 0;
        const std::uint32_t image_crc = fixed_encrypted_zero_crc(block, false);
        const std::uint32_t ecu_crc = fixed_encrypted_zero_crc(block, differs);
        logs.emplace_back(LogLevel::Info, std::format("FB{:02}\t0x{:08X}\t0x{:08X}", index, block.start, block.length));
        logs.emplace_back(LogLevel::Debug, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", ecu_crc, image_crc));
        logs.emplace_back(LogLevel::Info, std::format("\t{:08X}\t{:08X}", ecu_crc, image_crc));
        logs.emplace_back(LogLevel::Info, differs ? "\tNO" : "\tYES");
    }
    logs.emplace_back(LogLevel::Info, "Different blocks : ");
    if (first_block_mismatches)
    {
        logs.emplace_back(LogLevel::Info, "0, ");
    }
    logs.emplace_back(LogLevel::Info, first_block_mismatches ? " (total: 1)" : " (total: 0)");
}

std::vector<LogRecord> expected_legacy_write_logs(const flashdev_t& device, FlashOperation operation)
{
    const bool test_write = operation == FlashOperation::TestWrite;
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::Info, "Checking if Kernel already running...");
    logs.emplace_back(LogLevel::Info, "Requesting kernel ID");
    logs.emplace_back(LogLevel::Info, "Kernel ID: KID");
    append_legacy_compare_logs(logs, device, true, false);
    logs.emplace_back(LogLevel::Info, "--- Start writing ROM file to ECU flash memory ---");
    logs.emplace_back(LogLevel::Info, "Check max message length");
    logs.emplace_back(LogLevel::Info, ": 0x0200");
    logs.emplace_back(LogLevel::Info, "Check flashblock size");
    logs.emplace_back(LogLevel::Info, ": 0x1000");
    logs.emplace_back(LogLevel::Info, test_write ? "Test write mode on, no actual flash write is performed"
                                                 : "Test write mode off, perform actual flash write");
    logs.emplace_back(LogLevel::Error, "Flash mode succesfully set");
    logs.emplace_back(LogLevel::Info, "Flash block addr: 0x00000000 len: 0x00001000");
    logs.emplace_back(LogLevel::Info, "Check flash voltage");
    logs.emplace_back(LogLevel::Info, ": 2V");
    if (!test_write)
    {
        logs.emplace_back(LogLevel::Info, "Flash page erase addr: 0x00000000 len: 0x00001000");
        logs.emplace_back(LogLevel::Info, "Erasing flash page...");
        logs.emplace_back(LogLevel::Info, " erased");
    }
    logs.emplace_back(LogLevel::Info, "Start flash write addr: 0x00000000 len: 0x00001000");
    constexpr std::array<std::uint32_t, 8> kBufferAddresses{0x00000000, 0x00000200, 0x00000400, 0x00000600,
                                                            0x00000800, 0x00000A00, 0x00000C00, 0x00000E00};
    constexpr std::array<unsigned, 8> kPercents{0, 12, 25, 37, 50, 62, 75, 87};
    for (std::size_t index = 0; index < kBufferAddresses.size(); ++index)
    {
        logs.emplace_back(LogLevel::Debug, "Data written to flash buffer");
        logs.emplace_back(LogLevel::Info, std::format("Write flash buffer: 0x{:08X} ({}% - 512000 B/s, ~ 1 s)",
                                                      kBufferAddresses[index], kPercents[index]));
    }
    logs.emplace_back(LogLevel::Info, "Flash buffer write complete... ");
    logs.emplace_back(LogLevel::Debug, "Image CRC32: 0x9cc3cbd4");
    logs.emplace_back(LogLevel::Info, test_write ? "Validate flash addr: 0x0" : "Committ flash addr: 0x0");
    logs.emplace_back(LogLevel::Info, " len: 0x1000");
    logs.emplace_back(LogLevel::Info, " crc32: 0x9cc3cbd4");
    logs.emplace_back(LogLevel::Info, "Flash block ok");
    logs.emplace_back(LogLevel::Info, "Block 0 reflash complete.");
    append_legacy_compare_logs(logs, device, false, true);
    if (test_write)
    {
        logs.emplace_back(LogLevel::Info, "*** Test write PASS, it's ok to perform actual write! ***");
    }
    return logs;
}

std::vector<LogRecord> expected_legacy_read_logs(std::uint32_t rom_size)
{
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::Info, "Checking if Kernel already running...");
    logs.emplace_back(LogLevel::Info, "Requesting kernel ID");
    logs.emplace_back(LogLevel::Info, "Kernel ID: KID");
    logs.emplace_back(LogLevel::Info, "Start reading ROM, please wait...");
    for (std::uint32_t address = 0; address < rom_size; address += kReadPageSize)
    {
        // The controller-approved correction clamps FakeClock's zero elapsed
        // interval to one millisecond before applying legacy's speed formula.
        logs.emplace_back(
            LogLevel::Info,
            std::format("Kernel read addr: 0x{:08X} length: 0x00000400, 1024000 B/s {:>6} s", address, 1));
    }
    logs.emplace_back(LogLevel::Info, "ROM read ready");
    return logs;
}

std::vector<LogRecord> expected_legacy_upload_read_logs(std::uint32_t rom_size)
{
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::Info, "Checking if Kernel already running...");
    logs.emplace_back(LogLevel::Info, "Requesting kernel ID");
    logs.emplace_back(LogLevel::Error, "No valid response from ECU");
    logs.emplace_back(LogLevel::Info, "No response from kernel, continue initializing bootloader...");
    logs.emplace_back(LogLevel::Info, "Initializing bootloader");
    logs.emplace_back(LogLevel::Info, "Check if connected to bootloader");
    logs.emplace_back(LogLevel::Debug, "Connected to bootloader");
    logs.emplace_back(LogLevel::Debug, "Start address to upload kernel: ffff6004");
    logs.emplace_back(LogLevel::Info, "Set kernel upload address");
    logs.emplace_back(LogLevel::Debug, "Kernel load address set");
    logs.emplace_back(LogLevel::Info, "Uploading kernel, please wait...");
    logs.emplace_back(LogLevel::Debug, "Sending 1 blocks");
    logs.emplace_back(LogLevel::Debug, "All kernel blocks sent, checksum: 0xff");
    logs.emplace_back(LogLevel::Debug, "Verifying kernel checksum, please wait...");
    logs.emplace_back(LogLevel::Debug, "Checksum ok");
    logs.emplace_back(LogLevel::Info, "Kernel uploaded, jump to kernel");
    logs.emplace_back(LogLevel::Error, "No valid response from ECU");
    logs.emplace_back(LogLevel::Info, "Requesting kernel ID");
    logs.emplace_back(LogLevel::Info, "Kernel ID: KID");
    logs.emplace_back(LogLevel::Info, "Start reading ROM, please wait...");
    for (std::uint32_t address = 0; address < rom_size; address += kReadPageSize)
    {
        logs.emplace_back(
            LogLevel::Info,
            std::format("Kernel read addr: 0x{:08X} length: 0x00000400, 1024000 B/s {:>6} s", address, 1));
    }
    logs.emplace_back(LogLevel::Info, "ROM read ready");
    return logs;
}

// Kernel ID request literal, derived from legacy request_kernel_id() lines
// 1435-1484: [000007e0][beef][0001][01][000000]. This locks the helper above
// independently of the executor's private framing logic.
TEST(SubaruDensoSh705xDensoCanExecutor, KernelIdFrameMatchesHandDerivedWireBytes)
{
    EXPECT_THAT(kernel_id_request(),
                ElementsAre(0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00));
    EXPECT_THAT(iso_request(0x03, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x04, 0x00}),
                ElementsAre(0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00));
}

TEST(SubaruDensoSh705xDensoCanExecutor, TransportSetupUsesExactMixedCanConfigurationForEveryCatalogEntry)
{
    SubaruDensoSh705xDensoCanExecutor executor;
    for (const Case& test_case : kCases)
    {
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto setup = executor.transport_setup(*plan);
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

TEST(SubaruDensoSh705xDensoCanExecutor, AlreadyRunningKernelReadsEveryPageAndDecryptsTheRom)
{
    const Case& test_case = kCases.front();
    auto plan = read_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_live_read(transport, test_case, 0x00);
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), test_case.rom_size);
    // Hand-derived from the legacy encrypt/decrypt table at lines 1400-1429:
    // decrypting a 0x00000000 wire word with {6e86,f513,ce22,7856} yields
    // 0xeb14e86d. This is deliberately a literal rather than the executor's
    // helper, so the page-read trace independently locks its crypto direction.
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(4), ElementsAre(0xEB, 0x14, 0xE8, 0x6D));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.modeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::Iso15765Kernel}));
    EXPECT_EQ(events.logs, expected_legacy_read_logs(test_case.rom_size));
    EXPECT_FALSE(events.progress_calls.empty());
    ASSERT_FALSE(events.phase_progress_calls.empty());
    EXPECT_EQ(events.phase_progress_calls.front().phase_name, "Kernel");
    EXPECT_EQ(events.phase_progress_calls.front().phase_count, 2);
    EXPECT_EQ(events.phase_progress_calls.back().phase_name, "Read");
    EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(test_case.rom_size));
}

TEST(SubaruDensoSh705xDensoCanExecutor, ProbeTimeoutTransitionsThroughRawUploadAndBackBeforeReading)
{
    const Case& test_case = kCases.front();
    auto plan = read_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    transport.expectIsoWrite(kernel_id_request());
    transport.queueNoIsoFrame();
    script_upload(transport);
    transport.expectIsoWrite(kernel_id_request());
    transport.queueIsoRead(kernel_id_response());
    script_read_pages(transport, test_case.rom_size);
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.clear_receive_buffer_call_count_, 1);
    EXPECT_EQ(transport.modeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::Iso15765Kernel,
                                                                          ScriptedMixedCanMode::RawBootloader,
                                                                          ScriptedMixedCanMode::Iso15765Kernel}));
    EXPECT_THAT(clock.sleeps, Contains(3));
    EXPECT_THAT(clock.sleeps, Contains(1));
    EXPECT_THAT(clock.sleeps, Contains(200));
    EXPECT_EQ(events.logs, expected_legacy_upload_read_logs(test_case.rom_size));
}

TEST(SubaruDensoSh705xDensoCanExecutor, UploadPaddingAndChecksumUseHandDerivedSevenByteWireLiterals)
{
    const Case& test_case = kCases.front();
    auto plan = read_plan(test_case, bytes::Bytes{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77});
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    transport.expectIsoWrite(kernel_id_request());
    transport.queueNoIsoFrame();
    for (int count = 0; count < 1000; ++count)
    {
        transport.expectRawWrite(raw_request({0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    }
    transport.expectRawWrite(raw_request({0x7A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.queueRawRead(raw_response({0x7A, 0x96, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.expectRawWrite(raw_request({0x7A, 0x9C, 0xFF, 0xFF, 0x60, 0x04, 0x00, 0x00}));
    transport.queueRawRead(raw_response({0x7A, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    // Hand-derived r59f4e442 upload literals: seven data bytes become two
    // six-byte blocks and the checksum address is FFFF6004 + 0x0C + 1.
    transport.expectRawWrite(raw_request({0x7A, 0xAE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}));
    transport.expectRawWrite(raw_request({0x7A, 0xAE, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.expectRawWrite(raw_request({0x7A, 0xB4, 0xFF, 0xFF, 0x60, 0x11, 0x00, 0x00}));
    transport.queueRawRead(raw_response({0x7A, 0xB1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.expectRawWrite(raw_request({0x7A, 0x9C, 0xFF, 0xFF, 0x60, 0x04, 0x00, 0x00}));
    transport.queueRawRead(raw_response({0x7A, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.expectRawWrite(raw_request({0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.queueNoRawFrame();
    transport.expectIsoWrite(kernel_id_request());
    transport.queueIsoRead(kernel_id_response());
    transport.expectIsoWrite(iso_request(0x03, composeBe(0x00_b, u24(0), std::uint16_t{kReadPageSize})));
    transport.queueIsoRead(iso_response(0x43, bytes::Bytes{0x00}));
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, NonzeroLargeBlockUsesEightFixedCommitWindowsAndCumulativeWriteProgress)
{
    const Case& test_case = kCases.front();
    const flashdev_t *device = find_flash_device(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = write_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare_with_modified_blocks(transport, *device, {0, 8});
    script_flash_init(transport, false);
    script_first_flash_block(transport, false);
    script_large_nonzero_write_block(transport);
    script_compare(transport, *device, false);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    expect_two_block_write_progress(events);
}

TEST(SubaruDensoSh705xDensoCanExecutor, KernelIdProbeAndPostUploadVerificationWaitForTheLegacy200Milliseconds)
{
    const Case& test_case = kCases.front();
    auto plan = read_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    TimeoutRecordingMixedCanTransport transport;
    configure_and_open(executor, *plan, transport.scripted);
    transport.scripted.expectIsoWrite(kernel_id_request());
    transport.scripted.queueNoIsoFrame();
    script_upload(transport.scripted);
    transport.scripted.expectIsoWrite(kernel_id_request());
    transport.scripted.queueIsoRead(kernel_id_response());
    script_read_pages(transport.scripted, test_case.rom_size);
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 3), 1000);
    EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 1), 1);
    // upload_kernel() already owns two 200 ms waits (checksum and jump).
    // request_kernel_id() adds one before the initial probe and one before
    // the post-upload kernel-ID verification.
    EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 200), 4);
    EXPECT_TRUE(transport.scripted.scriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, CrcIntervalsAndFlashBufferAcknowledgementsUseTheLegacyTiming)
{
    const Case& test_case = kCases.front();
    const flashdev_t *device = find_flash_device(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = write_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    TimeoutRecordingMixedCanTransport transport;
    configure_and_open(executor, *plan, transport.scripted);
    script_kernel_alive(transport.scripted);
    script_compare(transport.scripted, *device, true);
    script_flash_init(transport.scripted, false);
    script_first_flash_block(transport.scripted, false);
    script_compare(transport.scripted, *device, false);
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    // get_changed_blocks() sleeps after every one of the sixteen CRC checks,
    // including the final check in each of the two comparison passes.
    EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 5), 32);
    EXPECT_TRUE(std::any_of(transport.iso_read_timeouts.begin(), transport.iso_read_timeouts.end(),
                            [](const auto& entry)
                            { return entry.first == std::optional<std::uint8_t>{0x22} && entry.second == 800; }));
    EXPECT_TRUE(transport.scripted.scriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, AllCatalogGeometriesUseFullPagedReads)
{
    SubaruDensoSh705xDensoCanExecutor executor;
    NeverCancelled cancellation;
    FakeClock clock;
    for (const Case& test_case : kCases)
    {
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_live_read(transport, test_case);
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << test_case.protocol << ": " << result.error().detail;
        ASSERT_TRUE(result->read_bytes.has_value());
        EXPECT_EQ(result->read_bytes->size(), test_case.rom_size);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, RejectsMalformedKernelResponseInsteadOfEnteringUncertainMode)
{
    auto plan = read_plan(kCases.front());
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    transport.expectIsoWrite(kernel_id_request());
    transport.queueIsoRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE, 0xEF, 0x00, 0x01});
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(transport.modeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::Iso15765Kernel}));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, RejectsBEEFResponseWhoseDeclaredLengthDoesNotCoverReadPayload)
{
    auto plan = read_plan(kCases.front());
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    transport.expectIsoWrite(iso_request(0x03, composeBe(0x00_b, u24(0), std::uint16_t{kReadPageSize})));
    bytes::Bytes truncated_declaration = iso_response(0x43, bytes::Bytes(kReadPageSize, 0x00));
    truncated_declaration[6] = 0x00;
    truncated_declaration[7] = 0x01;
    transport.queueIsoRead(std::move(truncated_declaration));
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, RawTransitionAndRawResponseFailuresAreFailClosed)
{
    auto plan = read_plan(kCases.front());
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    {
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectIsoWrite(kernel_id_request());
        transport.queueNoIsoFrame();
        transport.failNextRawTransition();
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Internal);
        EXPECT_EQ(transport.modeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::Iso15765Kernel}));
        EXPECT_TRUE(transport.scriptConsumed());
    }

    {
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectIsoWrite(kernel_id_request());
        transport.queueNoIsoFrame();
        for (int count = 0; count < 1000; ++count)
        {
            transport.expectRawWrite(raw_request({0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        }
        transport.expectRawWrite(raw_request({0x7A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        transport.queueRawRead(raw_response({0x7A, 0x96, 0, 0, 0, 0, 0, 0}, 0x22));
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, IsoTransitionFailureIsFailClosedAfterRawUpload)
{
    const Case& test_case = kCases.front();
    auto plan = read_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    transport.expectIsoWrite(kernel_id_request());
    transport.queueNoIsoFrame();
    script_upload(transport);
    transport.failNextIsoTransition();
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.modeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::Iso15765Kernel,
                                                                          ScriptedMixedCanMode::RawBootloader}));
}

TEST(SubaruDensoSh705xDensoCanExecutor, TimeoutAndDisconnectPropagateWithoutAnUnsafeFallback)
{
    const Case& test_case = kCases.front();
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    {
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        transport.expectIsoWrite(iso_request(0x03, composeBe(0x00_b, u24(0), std::uint16_t{kReadPageSize})));
        transport.queueNoIsoFrame();

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
        EXPECT_TRUE(transport.scriptConsumed());
    }

    {
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectIsoWrite(kernel_id_request());
        transport.queueIsoError(ErrorKind::Disconnected, "adapter dropped");

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_EQ(transport.modeChanges(), (std::vector<ScriptedMixedCanMode>{ScriptedMixedCanMode::Iso15765Kernel}));
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, MalformedJumpAcknowledgementIsToleratedBeforeTheIsoProbe)
{
    const Case& test_case = kCases.front();
    auto plan = read_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    transport.expectIsoWrite(kernel_id_request());
    transport.queueNoIsoFrame();
    script_upload(transport, raw_response({0x7A, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    script_kernel_alive(transport);
    script_read_pages(transport, test_case.rom_size);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(exact_log_count(events, LogLevel::Error, "Wrong response from ECU"), 1);
}

TEST(SubaruDensoSh705xDensoCanExecutor, ShortReadAndCrcPayloadsAreRejectedBeforeDecoding)
{
    const Case& test_case = kCases.front();
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    {
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        transport.expectIsoWrite(iso_request(0x03, composeBe(0x00_b, u24(0), std::uint16_t{kReadPageSize})));
        transport.queueIsoRead(iso_response(0x43, bytes::Bytes(kReadPageSize - 1, 0x00)));

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }

    {
        auto plan = write_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        const flashdev_t *device = find_flash_device(test_case.mcu);
        ASSERT_NE(device, nullptr);
        const MemoryRegion block{device->fblocks[0].start, device->fblocks[0].len};
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        transport.expectIsoWrite(iso_request(0x02, composeBe(block.start, 0x00_b, u24(block.length))));
        transport.queueIsoRead(iso_response(0x42, bytes::Bytes{0x00, 0x00, 0x00}));

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, ShortInitializationAndVoltagePayloadsAreRejectedBeforeDecoding)
{
    const Case& test_case = kCases.front();
    const flashdev_t *device = find_flash_device(test_case.mcu);
    ASSERT_NE(device, nullptr);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    {
        auto plan = write_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, *device, true);
        transport.expectIsoWrite(iso_request(0x05));
        transport.queueIsoRead(iso_response(0x45, bytes::Bytes{0x00, 0x00, 0x00}));

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }

    {
        auto plan = write_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, *device, true);
        script_flash_init(transport, false);
        transport.expectIsoWrite(iso_request(0x04));
        transport.queueIsoRead(iso_response(0x44, bytes::Bytes{0x00}));

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, ShortRawAndFlashAcknowledgementsAreRejectedBeforeUse)
{
    const Case& test_case = kCases.front();
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    {
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectIsoWrite(kernel_id_request());
        transport.queueNoIsoFrame();
        for (int count = 0; count < 1000; ++count)
        {
            transport.expectRawWrite(raw_request({0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        }
        transport.expectRawWrite(raw_request({0x7A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        transport.queueRawRead(raw_response({0x7A}));

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }

    {
        auto plan = write_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        const flashdev_t *device = find_flash_device(test_case.mcu);
        ASSERT_NE(device, nullptr);
        const bytes::Bytes encrypted_chunk = fixed_encrypted_zero_bytes(kWriteChunkSize);
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, *device, true);
        script_flash_init(transport, false);
        transport.expectIsoWrite(iso_request(0x04));
        transport.queueIsoRead(iso_response(0x44, bytes::Bytes{0x00, 0x64}));
        transport.expectIsoWrite(iso_request(0x25, composeBe(std::uint32_t{0})));
        transport.queueIsoRead(iso_response(0x65));
        transport.expectIsoWrite(iso_request(0x22, composeBe(std::uint32_t{0}, encrypted_chunk)));
        transport.queueIsoRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE, 0xEF, 0x00, 0x01});

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, PostEraseProtocolFailureEmitsTheLegacyRecoveryWarning)
{
    const Case& test_case = kCases.front();
    const flashdev_t *device = find_flash_device(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = write_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    const bytes::Bytes encrypted_chunk = fixed_encrypted_zero_bytes(kWriteChunkSize);
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare(transport, *device, true);
    script_flash_init(transport, false);
    transport.expectIsoWrite(iso_request(0x04));
    transport.queueIsoRead(iso_response(0x44, bytes::Bytes{0x00, 0x64}));
    transport.expectIsoWrite(iso_request(0x25, composeBe(std::uint32_t{0})));
    transport.queueIsoRead(iso_response(0x65));
    transport.expectIsoWrite(iso_request(0x22, composeBe(std::uint32_t{0}, encrypted_chunk)));
    transport.queueIsoRead(iso_response(0x7F));
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(exact_log_count(events, LogLevel::Error, kReflashRecoveryWarning), 1);
    ASSERT_FALSE(events.logs.empty());
    EXPECT_EQ(events.logs.back().first, LogLevel::Error);
    EXPECT_EQ(events.logs.back().second, kReflashRecoveryWarning);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, CancellationAfterEraseEmitsTheLegacyRecoveryWarning)
{
    const Case& test_case = kCases.front();
    const flashdev_t *device = find_flash_device(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = write_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    const bytes::Bytes encrypted_chunk = fixed_encrypted_zero_bytes(kWriteChunkSize);
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare(transport, *device, true);
    script_flash_init(transport, false);
    transport.expectIsoWrite(iso_request(0x04));
    transport.queueIsoRead(iso_response(0x44, bytes::Bytes{0x00, 0x64}));
    transport.expectIsoWrite(iso_request(0x25, composeBe(std::uint32_t{0})));
    transport.queueIsoRead(iso_response(0x65));
    transport.expectIsoWrite(iso_request(0x22, composeBe(std::uint32_t{0}, encrypted_chunk)));
    transport.queueIsoRead(iso_response(0x62));
    ToggleCancellation cancellation;
    FakeClock clock;
    CancellingEventSink events(cancellation);

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(exact_log_count(events, LogLevel::Error, kReflashRecoveryWarning), 1);
    ASSERT_FALSE(events.logs.empty());
    EXPECT_EQ(events.logs.back().first, LogLevel::Error);
    EXPECT_EQ(events.logs.back().second, kReflashRecoveryWarning);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, FailureBeforeEraseDoesNotEmitTheRecoveryWarning)
{
    const Case& test_case = kCases.front();
    const flashdev_t *device = find_flash_device(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = write_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare(transport, *device, true);
    script_flash_init(transport, false);
    transport.expectIsoWrite(iso_request(0x04));
    transport.queueIsoRead(iso_response(0x7F));
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(exact_log_count(events, LogLevel::Error, kReflashRecoveryWarning), 0);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh705xDensoCanExecutor, CancellationStopsWakeAndUploadAtTheirLoopBoundaries)
{
    const Case& test_case = kCases.front();
    auto plan = read_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    {
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectIsoWrite(kernel_id_request());
        transport.queueNoIsoFrame();
        transport.expectRawWrite(raw_request({0xFF, 0x86, 0, 0, 0, 0, 0, 0}));
        ToggleCancellation cancellation;
        CancellingClock clock(cancellation, 3);
        RecordingEventSink events;
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    }

    {
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectIsoWrite(kernel_id_request());
        transport.queueNoIsoFrame();
        for (int count = 0; count < 1000; ++count)
        {
            transport.expectRawWrite(raw_request({0xFF, 0x86, 0, 0, 0, 0, 0, 0}));
        }
        transport.expectRawWrite(raw_request({0x7A, 0x90, 0, 0, 0, 0, 0, 0}));
        transport.queueRawRead(raw_response({0x7A, 0x96, 0, 0, 0, 0, 0, 0}));
        const std::uint32_t address = test_case.kernel_address;
        transport.expectRawWrite(
            raw_request({0x7A, 0x9C, static_cast<bytes::Byte>(address >> 24), static_cast<bytes::Byte>(address >> 16),
                         static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address), 0, 0}));
        transport.queueRawRead(raw_response({0x7A, 0x9C, 0, 0, 0, 0, 0, 0}));
        transport.expectRawWrite(raw_request({0x7A, 0xAE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00}));
        ToggleCancellation cancellation;
        CancellingClock clock(cancellation, 1);
        RecordingEventSink events;
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, ReadAndWriteCancellationStopBeforeASecondTransfer)
{
    const Case& test_case = kCases.front();
    {
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        transport.expectIsoWrite(iso_request(0x03, composeBe(0x00_b, u24(0), std::uint16_t{kReadPageSize})));
        transport.queueIsoRead(iso_response(0x43, bytes::Bytes(kReadPageSize, 0)));
        ToggleCancellation cancellation;
        FakeClock clock;
        CancellingEventSink events(cancellation);
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_TRUE(transport.scriptConsumed());
    }

    {
        auto plan = write_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        const flashdev_t *device = find_flash_device(test_case.mcu);
        ASSERT_NE(device, nullptr);
        const bytes::Bytes encrypted_chunk = fixed_encrypted_zero_bytes(kWriteChunkSize);
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, *device, true);
        script_flash_init(transport, false);
        transport.expectIsoWrite(iso_request(0x04));
        transport.queueIsoRead(iso_response(0x44, bytes::Bytes{0x00, 0x64}));
        transport.expectIsoWrite(iso_request(0x25, composeBe(std::uint32_t{0})));
        transport.queueIsoRead(iso_response(0x65));
        transport.expectIsoWrite(iso_request(0x22, composeBe(std::uint32_t{0}, encrypted_chunk)));
        transport.queueIsoRead(iso_response(0x62));
        ToggleCancellation cancellation;
        FakeClock clock;
        CancellingEventSink events(cancellation);

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

void expect_legacy_write_logs(FlashOperation operation)
{
    const Case& test_case = kCases.front();
    const flashdev_t *device = find_flash_device(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = write_plan(test_case, operation);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh705xDensoCanExecutor executor;
    ScriptedMixedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare(transport, *device, true);
    script_flash_init(transport, operation == FlashOperation::TestWrite);
    script_first_flash_block(transport, operation == FlashOperation::TestWrite);
    script_compare(transport, *device, false);
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(events.logs, expected_legacy_write_logs(*device, operation));
}

TEST(SubaruDensoSh705xDensoCanExecutor, TestWriteOperatorLogsMatchTheCompleteLegacyRecord)
{
    expect_legacy_write_logs(FlashOperation::TestWrite);
}

TEST(SubaruDensoSh705xDensoCanExecutor, WriteOperatorLogsMatchTheCompleteLegacyRecord)
{
    expect_legacy_write_logs(FlashOperation::Write);
}

TEST(SubaruDensoSh705xDensoCanExecutor, WriteAndTestWriteUseCrcInitAndDistinctEraseCommitCommands)
{
    const Case& test_case = kCases.front();
    const flashdev_t *device = find_flash_device(test_case.mcu);
    ASSERT_NE(device, nullptr);
    for (const FlashOperation operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        auto plan = write_plan(test_case, operation);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, *device, true);
        script_flash_init(transport, operation == FlashOperation::TestWrite);
        script_first_flash_block(transport, operation == FlashOperation::TestWrite);
        script_compare(transport, *device, false);
        // The 59f4e442 oracle returns after the final comparison. In
        // particular, real-write has no trailing FLASH_DISABLE (0x21)
        // exchange; this script intentionally ends at verification so an
        // unsupported exchange fails the attempt.
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << result.error().detail;
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_EQ(exact_log_count(events, LogLevel::Error, kReflashRecoveryWarning), 0);
        expect_phase_trace(events, operation == FlashOperation::TestWrite ? "TestWrite" : "Write");
    }
}

TEST(SubaruDensoSh705xDensoCanExecutor, BoundedAttemptClosesOnceAndPreservesExecutionErrorOverCloseError)
{
    const Case& test_case = kCases.front();
    auto plan = read_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto executor = std::make_unique<SubaruDensoSh705xDensoCanExecutor>();
    auto transport = std::make_unique<ScriptedMixedCanFlashTransport>();
    ScriptedMixedCanFlashTransport *raw_transport = transport.get();
    script_live_read(*raw_transport, test_case);
    raw_transport->close_result_ = fail(ErrorKind::Internal, "close failed");
    auto attempt = bind_flash_attempt(std::move(*plan), std::move(executor), std::move(transport));
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = attempt->run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_EQ(raw_transport->closeCallCount(), 1);

    auto failing_plan = read_plan(test_case);
    ASSERT_TRUE(failing_plan.has_value()) << failing_plan.error().detail;
    auto failing_executor = std::make_unique<SubaruDensoSh705xDensoCanExecutor>();
    auto failing_transport = std::make_unique<ScriptedMixedCanFlashTransport>();
    ScriptedMixedCanFlashTransport *raw_failing_transport = failing_transport.get();
    raw_failing_transport->expectIsoWrite(kernel_id_request());
    raw_failing_transport->queueNoIsoFrame();
    raw_failing_transport->failNextRawTransition();
    raw_failing_transport->close_result_ = fail(ErrorKind::Timeout, "close timeout");
    auto failing_attempt =
        bind_flash_attempt(std::move(*failing_plan), std::move(failing_executor), std::move(failing_transport));

    auto execution_error = failing_attempt->run(clock, cancellation, events);

    ASSERT_FALSE(execution_error.has_value());
    EXPECT_EQ(execution_error.error().kind, ErrorKind::Internal);
    EXPECT_EQ(raw_failing_transport->closeCallCount(), 1);
    EXPECT_TRUE(has_log(events, LogLevel::Warning, "close failed after execution error"));
}

} // namespace
} // namespace fastecu::flash
