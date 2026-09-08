#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
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
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
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

constexpr std::uint32_t kRequestId = 0x7E1;
constexpr std::uint32_t kResponseId = 0x7E9;
constexpr std::uint32_t kReadPageSize = 0x400;
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitBlockSize = 0x1000;
constexpr std::string_view kReflashRecoveryWarning =
    "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most likely still running and "
    "receiving commands!";

struct Case
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t rom_size;
    std::uint32_t kernel_address;
};

constexpr std::array<Case, 2> kCases{{
    {"sub_tcu_denso_sh7055_can", "SH7055", 0x00080000, 0xFFFF9000},
    {"sub_tcu_denso_sh7058_can", "SH7058", 0x00100000, 0xFFFF3000},
}};

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

    void log(LogLevel level, std::string_view message) override
    {
        RecordingEventSink::log(level, message);
        if (message == " erased")
        {
            cancellation_.cancel();
        }
    }

  private:
    ToggleCancellation& cancellation_;
};

KernelImage kernel_for(const Case& test_case, bytes::Bytes data = {0x01, 0x02, 0x03, 0x04})
{
    return {.id = "tcu-denso-kernel", .load_address = test_case.kernel_address, .bytes = std::move(data)};
}

Result<FlashPlan> read_plan(const Case& test_case, bytes::Bytes kernel = {0x01, 0x02, 0x03, 0x04})
{
    return build_subaru_tcu_denso_sh705x_can_plan(FlashOperation::Read, test_case.protocol, test_case.mcu, std::nullopt,
                                                  kernel_for(test_case, std::move(kernel)));
}

Result<FlashPlan> write_plan(const Case& test_case, bytes::Bytes image = {})
{
    if (image.empty())
    {
        image.assign(test_case.rom_size, bytes::Byte{0});
    }
    return build_subaru_tcu_denso_sh705x_can_plan(FlashOperation::Write, test_case.protocol, test_case.mcu,
                                                  std::move(image), kernel_for(test_case));
}

bytes::Bytes request(bytes::ByteView pdu)
{
    return composeBe(kRequestId, pdu);
}

bytes::Bytes response(bytes::ByteView pdu)
{
    return composeBe(kResponseId, pdu);
}

// Legacy request_kernel_id(), r59f4e442 lines 1583-1647. This is the initial
// ISO-15765 probe's exact twelve-byte wire buffer, not a UDS PDU.
bytes::Bytes kernel_id_request()
{
    return {0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

bytes::Bytes beef_request(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes message = composeBe(kRequestId, std::uint16_t{0xBEEF}, static_cast<std::uint16_t>(payload.size() + 1),
                                     bytes::Byte(opcode));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

bytes::Bytes beef_response(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes message = composeBe(kResponseId, std::uint16_t{0xBEEF}, static_cast<std::uint16_t>(payload.size() + 1),
                                     bytes::Byte(opcode));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

bytes::Bytes kernel_id_response(bool with_transport_padding = false)
{
    bytes::Bytes response = beef_response(0x41, bytes::Bytes{'K', 'I', 'D'});
    if (with_transport_padding)
    {
        response.insert(response.end(), {0x00, 0x00});
    }
    return response;
}

void configure_and_open(SubaruTcuDensoSh705xCanExecutor& executor, const FlashPlan& plan,
                        ScriptedCanFlashTransport& transport)
{
    auto setup = executor.transport_setup(plan);
    ASSERT_TRUE(setup.has_value()) << setup.error().detail;
    ASSERT_TRUE(transport.configure(*setup).has_value());
    ASSERT_TRUE(transport.open().has_value());
}

void script_kernel_alive(ScriptedCanFlashTransport& transport, bool with_transport_padding = false)
{
    transport.expectWrite(kernel_id_request());
    transport.queueRead(kernel_id_response(with_transport_padding));
}

void script_kernel_probe_timeout(ScriptedCanFlashTransport& transport)
{
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        transport.expectWrite(kernel_id_request());
        transport.queue_no_frame();
    }
}

void script_read_pages(ScriptedCanFlashTransport& transport, std::uint32_t size, bytes::Byte wire_fill = 0)
{
    const bytes::Bytes encrypted_zero_word{0xE7, 0xE2, 0x14, 0x30};
    for (std::uint32_t address = 0; address < size; address += kReadPageSize)
    {
        transport.expectWrite(beef_request(0x03, composeBe(0x00_b, u24(address), std::uint16_t{kReadPageSize})));
        bytes::Bytes encrypted_page(kReadPageSize, wire_fill);
        if (wire_fill == 0)
        {
            for (std::size_t offset = 0; offset < encrypted_page.size(); offset += encrypted_zero_word.size())
            {
                std::copy(encrypted_zero_word.begin(), encrypted_zero_word.end(), encrypted_page.begin() + offset);
            }
        }
        transport.queueRead(beef_response(0x43, encrypted_page));
    }
}

void script_identity_queries(ScriptedCanFlashTransport& transport)
{
    // connect_bootloader(), r59f4e442 lines 137-214: both identity queries
    // are non-fatal, but successful read identity contributes to rom_id.
    transport.expectWrite(request(bytes::Bytes{0xAA}));
    transport.queueRead(response(bytes::Bytes{0xEA, 0x00, 0x00, 0x00, 'E', 'C', 'U', '0', '1'}));
    transport.expectWrite(request(bytes::Bytes{0x09, 0x04}));
    transport.queueRead(response(bytes::Bytes{0x49, 0x04, 0x00, 'C', 'A', 'L'}));
}

void script_strict_session_and_security(ScriptedCanFlashTransport& transport)
{
    // connect_bootloader(), r59f4e442 lines 216-359. Seed 11 22 33 44 maps
    // to 35 B6 83 BF; the literal is fixed independently of executor code.
    transport.expectWrite(request(bytes::Bytes{0x10, 0x03}));
    transport.queueRead(response(bytes::Bytes{0x50, 0x03}));
    transport.expectWrite(request(bytes::Bytes{0x27, 0x01}));
    transport.queueRead(response(bytes::Bytes{0x67, 0x01, 0x11, 0x22, 0x33, 0x44}));
    transport.expectWrite(request(bytes::Bytes{0x27, 0x02, 0x35, 0xB6, 0x83, 0xBF}));
    transport.queueRead(response(bytes::Bytes{0x67, 0x02}));
    transport.expectWrite(request(bytes::Bytes{0x10, 0x02}));
    transport.queueRead(response(bytes::Bytes{0x50, 0x42}));
}

void script_kernel_upload(ScriptedCanFlashTransport& transport, const Case& test_case)
{
    // upload_kernel(), r59f4e442 lines 369-627. Four kernel bytes pad to one
    // 128-byte transfer. The checksum word is 59 A3 A2 56, and the first and
    // last encrypted words are fixed here so the test never calls production
    // crypto helpers.
    transport.expectWrite(request(composeBe(0x34_b, 0x04_b, 0x33_b, u24(test_case.kernel_address), u24(0x80))));
    transport.queueRead(response(bytes::Bytes{0x74, 0x20}));

    bytes::Bytes block{0xB6,
                       static_cast<bytes::Byte>(test_case.kernel_address >> 16),
                       static_cast<bytes::Byte>(test_case.kernel_address >> 8),
                       static_cast<bytes::Byte>(test_case.kernel_address),
                       0xA1,
                       0xB0,
                       0x3A,
                       0xDD};
    for (int word = 0; word < 30; ++word)
    {
        block.insert(block.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    block.insert(block.end(), {0x6C, 0x78, 0x52, 0x90});
    transport.expectWrite(request(block));
    transport.queue_no_frame(); // Legacy reads but does not validate transfer-block replies.
    transport.expectWrite(request(bytes::Bytes{0xB6, static_cast<bytes::Byte>((test_case.kernel_address + 0x80) >> 16),
                                               static_cast<bytes::Byte>((test_case.kernel_address + 0x80) >> 8),
                                               static_cast<bytes::Byte>(test_case.kernel_address + 0x80)}));
    transport.queue_no_frame(); // The <= maxblocks loop emits one empty final block.

    transport.expectWrite(request(bytes::Bytes{0x37}));
    transport.queueRead(response(bytes::Bytes{0x77}));
    transport.expectWrite(request(bytes::Bytes{0x31, 0x01, 0x02, 0x02, 0x02}));
    transport.queueRead(response(bytes::Bytes{0x71, 0x01, 0x02, 0x02, 0x02}));
}

bytes::Bytes zero_chunk(std::size_t size)
{
    return bytes::Bytes(size, bytes::Byte{0});
}

std::uint32_t plain_zero_crc(std::uint32_t length, bool mismatch)
{
    switch (length)
    {
    case 0x00001000:
        return mismatch ? 0xF722EF48U : 0xF722EF49U;
    case 0x00008000:
        return mismatch ? 0xE5FD2EA3U : 0xE5FD2EA2U;
    case 0x00010000:
        return mismatch ? 0xDFEA015CU : 0xDFEA015DU;
    case 0x00018000:
        return mismatch ? 0xC85F0DECU : 0xC85F0DEDU;
    case 0x00020000:
        return mismatch ? 0xC39BE4A9U : 0xC39BE4A8U;
    default:
        ADD_FAILURE() << "missing fixed TCU CRC fixture for length " << length;
        return 0;
    }
}

void script_crc(ScriptedCanFlashTransport& transport, const MemoryRegion& block, std::uint32_t crc)
{
    transport.expectWrite(beef_request(0x02, composeBe(block.start, 0x00_b, u24(block.length))));
    transport.queueRead(beef_response(0x42, composeBe(crc)));
    transport.queue_no_frame(); // Legacy drains a short stale frame after each CRC comparison.
}

void script_compare(ScriptedCanFlashTransport& transport, const flashdev_t& device, bool first_block_mismatches)
{
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion block{device.fblocks[index].start, device.fblocks[index].len};
        script_crc(transport, block, plain_zero_crc(block.length, first_block_mismatches && index == 0));
    }
}

void script_flash_init(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(beef_request(0x05));
    transport.queueRead(beef_response(0x45, composeBe(std::uint32_t{0x00000200})));
    transport.expectWrite(beef_request(0x06));
    transport.queueRead(beef_response(0x46, composeBe(std::uint32_t{0x00001000})));
    transport.expectWrite(beef_request(0x20));
    transport.queueRead(beef_response(0x60));
}

void script_first_flash_block(ScriptedCanFlashTransport& transport, bool include_chunks = true)
{
    transport.expectWrite(beef_request(0x04));
    transport.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
    transport.expectWrite(beef_request(0x25, composeBe(std::uint32_t{0x00000000})));
    transport.queueRead(beef_response(0x65));
    if (!include_chunks)
    {
        return;
    }
    const bytes::Bytes chunk = zero_chunk(kWriteChunkSize);
    for (std::uint32_t offset = 0; offset < kCommitBlockSize; offset += kWriteChunkSize)
    {
        transport.expectWrite(beef_request(0x22, composeBe(offset, chunk)));
        transport.queueRead(beef_response(0x62));
    }
    transport.expectWrite(
        beef_request(0x24, composeBe(std::uint32_t{0x00000000}, std::uint16_t{kCommitBlockSize}, 0xF722EF49U)));
    transport.queueRead(beef_response(0x64));
}

bool has_log(const RecordingEventSink& events, LogLevel level, std::string_view text)
{
    return std::any_of(events.logs.begin(), events.logs.end(), [level, text](const auto& log)
                       { return log.first == level && log.second.find(text) != std::string::npos; });
}

using LogRecord = std::pair<LogLevel, std::string>;

void append_compare_logs(std::vector<LogRecord>& logs, const flashdev_t& device, bool first_block_mismatches,
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
        const std::uint32_t image_crc = plain_zero_crc(block.length, false);
        const std::uint32_t ecu_crc = plain_zero_crc(block.length, differs);
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

std::vector<LogRecord> expected_write_logs(const flashdev_t& device)
{
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::Info, "Checking if kernel is already running...");
    logs.emplace_back(LogLevel::Info, "Requesting kernel ID");
    logs.emplace_back(LogLevel::Info, "Kernel ID: KID");
    append_compare_logs(logs, device, true, false);
    logs.emplace_back(LogLevel::Info, "--- Start writing ROM file to ECU flash memory ---");
    logs.emplace_back(LogLevel::Info, "Check max message length");
    logs.emplace_back(LogLevel::Info, ": 0x0200");
    logs.emplace_back(LogLevel::Info, "Check flashblock size");
    logs.emplace_back(LogLevel::Info, ": 0x1000");
    logs.emplace_back(LogLevel::Info, "Test write mode off, perform actual flash write");
    logs.emplace_back(LogLevel::Error, "Flash mode succesfully set");
    logs.emplace_back(LogLevel::Info, "Flash block addr: 0x00000000 len: 0x00001000");
    logs.emplace_back(LogLevel::Info, "Check flash voltage");
    logs.emplace_back(LogLevel::Info, ": 2V");
    logs.emplace_back(LogLevel::Info, "Flash page erase addr: 0x00000000 len: 0x00001000");
    logs.emplace_back(LogLevel::Info, "Erasing flash page...");
    logs.emplace_back(LogLevel::Info, " erased");
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
    logs.emplace_back(LogLevel::Debug, "Image CRC32: 0xf722ef49");
    logs.emplace_back(LogLevel::Info, "Committ flash addr: 0x0");
    logs.emplace_back(LogLevel::Info, " len: 0x1000");
    logs.emplace_back(LogLevel::Info, " crc32: 0xf722ef49");
    logs.emplace_back(LogLevel::Info, "Flash block ok");
    logs.emplace_back(LogLevel::Info, "Block 0 reflash complete.");
    append_compare_logs(logs, device, false, true);
    return logs;
}

std::vector<LogRecord> expected_read_logs(const Case& test_case)
{
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::Info, "Checking if kernel is already running...");
    logs.emplace_back(LogLevel::Info, "Requesting kernel ID");
    logs.emplace_back(LogLevel::Info, "Kernel ID: KID");
    logs.emplace_back(LogLevel::Info, "Start reading ROM, please wait...");
    constexpr unsigned kFakeClockSpeed = 1'024'000;
    for (std::uint32_t offset = 0; offset < test_case.rom_size; offset += kReadPageSize)
    {
        const unsigned time_left = ((test_case.rom_size - offset) / kFakeClockSpeed) % 9999U + 1U;
        logs.emplace_back(LogLevel::Info, std::format("Kernel read addr: 0x{:08X} length: 0x{:08X}, {:>6} B/s {:>6} s",
                                                      offset, kReadPageSize, kFakeClockSpeed, time_left));
    }
    logs.emplace_back(LogLevel::Info, "ROM read ready");
    return logs;
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelIdFrameMatchesHandDerivedWireBytes)
{
    EXPECT_THAT(kernel_id_request(),
                ElementsAre(0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));
    EXPECT_THAT(beef_request(0x03, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x04, 0x00}),
                ElementsAre(0x00, 0x00, 0x07, 0xE1, 0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00));
}

TEST(SubaruTcuDensoSh705xCanExecutor, TransportSetupUsesExactTcuIsoConfiguration)
{
    SubaruTcuDensoSh705xCanExecutor executor;
    for (const Case& test_case : kCases)
    {
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto setup = executor.transport_setup(*plan);
        ASSERT_TRUE(setup.has_value()) << setup.error().detail;
        EXPECT_EQ(setup->bitrate, 500000);
        EXPECT_EQ(setup->request_id, kRequestId);
        EXPECT_EQ(setup->response_id, kResponseId);
        EXPECT_FALSE(setup->extended_id);
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, AlreadyRunningKernelReadsBothMcuGeometriesAndDecryptsRom)
{
    SubaruTcuDensoSh705xCanExecutor executor;
    NeverCancelled cancellation;
    for (const Case& test_case : kCases)
    {
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport, test_case.mcu == "SH7058");
        script_read_pages(transport, test_case.rom_size);
        RecordingClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << test_case.protocol << ": " << result.error().detail;
        ASSERT_TRUE(result->read_bytes.has_value());
        EXPECT_EQ(result->read_bytes->size(), test_case.rom_size);
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(4), ElementsAre(0x00, 0x00, 0x00, 0x00));
        EXPECT_FALSE(result->rom_id.has_value());
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_EQ(events.logs, expected_read_logs(test_case));
        EXPECT_THAT(events.phase_progress_calls.front().phase_name, "Kernel");
        EXPECT_THAT(events.phase_progress_calls.back().phase_name, "Read");
        EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(test_case.rom_size));
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, ProbeTimeoutRunsIdentityStrictUdsUploadAndPostUploadBeefRead)
{
    const Case& test_case = kCases[1];
    auto plan = read_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_probe_timeout(transport);
    script_identity_queries(transport);
    script_strict_session_and_security(transport);
    script_kernel_upload(transport, test_case);
    transport.expectWrite(kernel_id_request());
    transport.queueRead(kernel_id_response());
    script_read_pages(transport, kReadPageSize);
    transport.expectWrite(beef_request(0x03, composeBe(0x00_b, u24(kReadPageSize), std::uint16_t{kReadPageSize})));
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x01, 0x43});
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_THAT(clock.sleeps, Contains(50));
    EXPECT_THAT(clock.sleeps, Contains(500));
    EXPECT_TRUE(has_log(events, LogLevel::Info, "ECU ID: ECU01"));
    EXPECT_TRUE(has_log(events, LogLevel::Info, "CAL ID: CAL"));
}

TEST(SubaruTcuDensoSh705xCanExecutor, WriteComparesErasesProgramsAndVerifiesFirstChangedBlock)
{
    const Case& test_case = kCases[1];
    const flashdev_t *device = find_flash_device(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = write_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare(transport, *device, true);
    script_flash_init(transport);
    script_first_flash_block(transport);
    script_compare(transport, *device, false);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(events.logs, expected_write_logs(*device));
    ASSERT_FALSE(events.phase_progress_calls.empty());
    EXPECT_EQ(events.phase_progress_calls.front().phase_name, "Kernel");
    EXPECT_EQ(events.phase_progress_calls.back().phase_name, "Complete");
}

TEST(SubaruTcuDensoSh705xCanExecutor, RejectsInvalidPlanBeforeTransportInteraction)
{
    auto fields = FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruTcuDensoSh705xCan,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(kCases[0].protocol),
        .mcu_name = std::string(kCases[0].mcu),
        .transfer_region = {0, kCases[0].rom_size},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = kernel_for(kCases[0]),
        .family_plan = SubaruTcuDensoSh705xCanPlan{.request_id = 0x7E0},
        .confirmations = {},
    };
    auto plan = validate_and_build(std::move(fields));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

TEST(SubaruTcuDensoSh705xCanExecutor, MalformedTimeoutNegativeAndDisconnectRepliesAreTyped)
{
    auto plan = read_plan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    NeverCancelled cancellation;

    {
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectWrite(kernel_id_request());
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x01});
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    }
    {
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_probe_timeout(transport);
        script_identity_queries(transport);
        transport.expectWrite(request(bytes::Bytes{0x10, 0x03}));
        transport.queueRead(response(bytes::Bytes{0x7F, 0x10, 0x22}));
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    }
    {
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectWrite(kernel_id_request());
        transport.queue_error(ErrorKind::Disconnected, "adapter dropped");
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, CancellationAfterEraseReturnsRecoveryWarning)
{
    const Case& test_case = kCases[1];
    const flashdev_t *device = find_flash_device(test_case.mcu);
    ASSERT_NE(device, nullptr);
    auto plan = write_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare(transport, *device, true);
    script_flash_init(transport);
    script_first_flash_block(transport, false);
    ToggleCancellation cancellation;
    FakeClock clock;
    CancellingEventSink events(cancellation);

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(has_log(events, LogLevel::Error, kReflashRecoveryWarning));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuDensoSh705xCanExecutor, BoundAttemptReturnsCloseErrorOnlyWhenExecutionSucceeds)
{
    auto plan = read_plan(kCases[0]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    auto success_transport = std::make_unique<ScriptedCanFlashTransport>();
    script_kernel_alive(*success_transport);
    script_read_pages(*success_transport, kCases[0].rom_size);
    success_transport->close_result_ = fail(ErrorKind::Disconnected, "close failed");
    auto success_attempt =
        bind_flash_attempt(*plan, std::make_unique<SubaruTcuDensoSh705xCanExecutor>(), std::move(success_transport));
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto close_only = success_attempt->run(clock, cancellation, events);

    ASSERT_FALSE(close_only.has_value());
    EXPECT_EQ(close_only.error().kind, ErrorKind::Disconnected);

    auto failing_transport = std::make_unique<ScriptedCanFlashTransport>();
    script_kernel_alive(*failing_transport);
    failing_transport->expectWrite(beef_request(0x03, composeBe(0x00_b, u24(0), std::uint16_t{kReadPageSize})));
    failing_transport->queue_no_frame();
    failing_transport->close_result_ = fail(ErrorKind::Internal, "close also failed");
    auto failing_attempt =
        bind_flash_attempt(*plan, std::make_unique<SubaruTcuDensoSh705xCanExecutor>(), std::move(failing_transport));
    RecordingEventSink failing_events;

    auto execution_error = failing_attempt->run(clock, cancellation, failing_events);

    ASSERT_FALSE(execution_error.has_value());
    EXPECT_EQ(execution_error.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(has_log(failing_events, LogLevel::Warning, "close failed after execution error"));
}

} // namespace
} // namespace fastecu::flash
