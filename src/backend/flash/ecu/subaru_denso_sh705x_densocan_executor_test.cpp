#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
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
        // PhaseReporter invokes phase_progress() directly. Do not route this
        // bookkeeping callback through progress(), otherwise cancellation
        // fires after the Kernel phase instead of after an actual transfer.
        phase_progress_calls.push_back(
            {std::string(event.phase_name), event.phase_index, event.phase_count, event.done, event.total});
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

void script_upload(ScriptedMixedCanFlashTransport& transport, const KernelImage& kernel,
                   std::optional<cdbg::CanFrame> jump_response = std::nullopt)
{
    for (int count = 0; count < 1000; ++count)
    {
        transport.expectRawWrite(raw_request({0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    }
    transport.expectRawWrite(raw_request({0x7A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    transport.queueRawRead(raw_response({0x7A, 0x96, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    const auto address = kernel.load_address;
    transport.expectRawWrite(
        raw_request({0x7A, 0x9C, static_cast<bytes::Byte>(address >> 24), static_cast<bytes::Byte>(address >> 16),
                     static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address), 0x00, 0x00}));
    transport.queueRawRead(raw_response({0x7A, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    bytes::Bytes padded = kernel.bytes;
    padded.resize((padded.size() + 5U) / 6U * 6U, 0);
    for (std::size_t offset = 0; offset < padded.size(); offset += 6)
    {
        bytes::Bytes block{0x7A, 0xAE};
        block.insert(block.end(), padded.begin() + static_cast<std::ptrdiff_t>(offset),
                     padded.begin() + static_cast<std::ptrdiff_t>(offset + 6));
        transport.expectRawWrite(raw_request(std::move(block)));
    }

    const std::uint32_t end_plus_one = address + static_cast<std::uint32_t>(padded.size()) + 1;
    transport.expectRawWrite(raw_request(
        {0x7A, 0xB4, static_cast<bytes::Byte>(end_plus_one >> 24), static_cast<bytes::Byte>(end_plus_one >> 16),
         static_cast<bytes::Byte>(end_plus_one >> 8), static_cast<bytes::Byte>(end_plus_one), 0x00, 0x00}));
    transport.queueRawRead(raw_response({0x7A, 0xB1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    transport.expectRawWrite(
        raw_request({0x7A, 0x9C, static_cast<bytes::Byte>(address >> 24), static_cast<bytes::Byte>(address >> 16),
                     static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address), 0x00, 0x00}));
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

bytes::Bytes encrypt_payload(bytes::ByteView image)
{
    constexpr std::array<std::uint16_t, 4> kEncrypt{0x7856, 0xCE22, 0xF513, 0x6E86};
    constexpr std::array<std::uint8_t, 32> kTransform{
        0x05, 0x06, 0x07, 0x01, 0x09, 0x0C, 0x0D, 0x08, 0x0A, 0x0D, 0x02, 0x0B, 0x0F, 0x04, 0x00, 0x03,
        0x0B, 0x04, 0x06, 0x00, 0x0F, 0x02, 0x0D, 0x09, 0x05, 0x0C, 0x01, 0x0A, 0x03, 0x0D, 0x0E, 0x08,
    };
    return SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()), kEncrypt, kTransform);
}

void script_crc(ScriptedMixedCanFlashTransport& transport, const MemoryRegion& block, std::uint32_t crc)
{
    transport.expectIsoWrite(iso_request(0x02, composeBe(block.start, 0x00_b, u24(block.length))));
    transport.queueIsoRead(iso_response(0x42, composeBe(crc)));
    // Legacy check_romcrc() discards a short response after either comparison result.
    transport.queueNoIsoFrame();
}

void script_compare(ScriptedMixedCanFlashTransport& transport, const flashdev_t& device, bytes::ByteView wire_image,
                    bool first_block_mismatches)
{
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion block{device.fblocks[index].start, device.fblocks[index].len};
        std::uint32_t crc = checksum::crc32(wire_image.subspan(block.start, block.length));
        if (first_block_mismatches && index == 0)
        {
            crc ^= 0x00000001U;
        }
        script_crc(transport, block, crc);
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

void script_first_flash_block(ScriptedMixedCanFlashTransport& transport, bytes::ByteView wire_image, bool test_write)
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

    for (std::uint32_t offset = 0; offset < kCommitBlockSize; offset += kWriteChunkSize)
    {
        transport.expectIsoWrite(iso_request(0x22, composeBe(offset, wire_image.subspan(offset, kWriteChunkSize))));
        transport.queueIsoRead(iso_response(0x62));
    }
    const std::uint32_t crc = checksum::crc32(wire_image.first(kCommitBlockSize));
    transport.expectIsoWrite(
        iso_request(test_write ? 0x23 : 0x24, composeBe(start, std::uint16_t{kCommitBlockSize}, crc)));
    transport.queueIsoRead(iso_response(test_write ? 0x63 : 0x64));
}

bool has_log(const RecordingEventSink& events, LogLevel level, std::string_view text)
{
    return std::any_of(events.logs.begin(), events.logs.end(), [level, text](const auto& log)
                       { return log.first == level && log.second.find(text) != std::string::npos; });
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
    EXPECT_TRUE(has_log(events, LogLevel::Info, "kernel"));
    EXPECT_TRUE(has_log(events, LogLevel::Info, "reading"));
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
    script_upload(transport, *plan->kernel());
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
    script_upload(transport, *plan->kernel());
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
    script_upload(transport, *plan->kernel(), raw_response({0x7A, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    script_kernel_alive(transport);
    script_read_pages(transport, test_case.rom_size);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(has_log(events, LogLevel::Warning, "Unexpected kernel-jump response"));
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
        const bytes::Bytes wire_image = encrypt_payload(*plan->image());
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, *device, wire_image, true);
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
        const bytes::Bytes wire_image = encrypt_payload(*plan->image());
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, *device, wire_image, true);
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
        const bytes::Bytes wire_image = encrypt_payload(*plan->image());
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, *device, wire_image, true);
        script_flash_init(transport, false);
        transport.expectIsoWrite(iso_request(0x04));
        transport.queueIsoRead(iso_response(0x44, bytes::Bytes{0x00, 0x64}));
        transport.expectIsoWrite(iso_request(0x25, composeBe(std::uint32_t{0})));
        transport.queueIsoRead(iso_response(0x65));
        transport.expectIsoWrite(
            iso_request(0x22, composeBe(std::uint32_t{0}, bytes::ByteView(wire_image).first(kWriteChunkSize))));
        transport.queueIsoRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE, 0xEF, 0x00, 0x01});

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }
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
        const bytes::Bytes wire_image = encrypt_payload(*plan->image());
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, *device, wire_image, true);
        script_flash_init(transport, false);
        transport.expectIsoWrite(iso_request(0x04));
        transport.queueIsoRead(iso_response(0x44, bytes::Bytes{0x00, 0x64}));
        transport.expectIsoWrite(iso_request(0x25, composeBe(std::uint32_t{0})));
        transport.queueIsoRead(iso_response(0x65));
        transport.expectIsoWrite(
            iso_request(0x22, composeBe(std::uint32_t{0}, bytes::ByteView(wire_image).first(kWriteChunkSize))));
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

TEST(SubaruDensoSh705xDensoCanExecutor, WriteAndTestWriteUseCrcInitAndDistinctEraseCommitCommands)
{
    const Case& test_case = kCases.front();
    const flashdev_t *device = find_flash_device(test_case.mcu);
    ASSERT_NE(device, nullptr);
    for (const FlashOperation operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        auto plan = write_plan(test_case, operation);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        const bytes::Bytes wire_image = encrypt_payload(*plan->image());
        SubaruDensoSh705xDensoCanExecutor executor;
        ScriptedMixedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, *device, wire_image, true);
        script_flash_init(transport, operation == FlashOperation::TestWrite);
        script_first_flash_block(transport, wire_image, operation == FlashOperation::TestWrite);
        script_compare(transport, *device, wire_image, false);
        if (operation == FlashOperation::Write)
        {
            transport.expectIsoWrite(iso_request(0x21));
            transport.queueIsoRead(iso_response(0x61));
        }
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << result.error().detail;
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_TRUE(
            has_log(events, LogLevel::Info, operation == FlashOperation::TestWrite ? "Test write" : "Comparing ECU"));
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
