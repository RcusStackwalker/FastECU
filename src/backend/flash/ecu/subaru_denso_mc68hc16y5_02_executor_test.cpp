#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.h"

#include <gtest/gtest.h>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/flash/eeprom/eeprom_read_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash
{
namespace
{
using namespace bytes;
using namespace bytes::literals;

class NeverCancelled : public ICancellationToken
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
        return cancelled_;
    }

    void cancel()
    {
        cancelled_ = true;
    }

  private:
    bool cancelled_ = false;
};

class ShortWriteTransport final : public ScriptedKlineFlashTransport
{
  public:
    Result<std::size_t> write(bytes::ByteView data) override
    {
        return data.empty() ? 0u : data.size() - 1;
    }
};

class DrainCancellingTransport final : public ScriptedKlineFlashTransport
{
  public:
    explicit DrainCancellingTransport(ToggleCancellation& cancellation) : cancellation_(cancellation)
    {
    }

    Result<OptionalBytes> read(int timeout_ms, const ICancellationToken& cancellation) override
    {
        if (timeout_ms == 10)
        {
            cancellation_.cancel();
        }
        return ScriptedKlineFlashTransport::read(timeout_ms, cancellation);
    }

    Result<std::size_t> write(bytes::ByteView data) override
    {
        write_attempts_.emplace_back(data.begin(), data.end());
        return ScriptedKlineFlashTransport::write(data);
    }

    std::vector<bytes::Bytes> write_attempts_;

  private:
    ToggleCancellation& cancellation_;
};

class CancelAfterEraseTransport final : public ScriptedKlineFlashTransport
{
  public:
    explicit CancelAfterEraseTransport(ToggleCancellation& cancellation)
        : cancellation_(cancellation)
    {
    }

    Result<std::size_t> write(bytes::ByteView data) override
    {
        if (data.size() > 4 && data[0] == 0xBE && data[1] == 0xEF)
        {
            erase_response_pending_ = data[4] == 0x25;
            if (data[4] == 0x22)
            {
                ++flash_buffer_write_attempts_;
            }
        }
        return ScriptedKlineFlashTransport::write(data);
    }

    Result<OptionalBytes> read(int timeout_ms, const ICancellationToken& cancellation) override
    {
        Result<OptionalBytes> result =
            ScriptedKlineFlashTransport::read(timeout_ms, cancellation);
        if (erase_response_pending_)
        {
            erase_response_pending_ = false;
            cancellation_.cancel();
        }
        return result;
    }

    std::size_t flash_buffer_write_attempts_ = 0;

  private:
    ToggleCancellation& cancellation_;
    bool erase_response_pending_ = false;
};

Result<FlashPlan> stock_plan(FlashOperation operation = FlashOperation::Read)
{
    return build_subaru_denso_mc68hc16y5_02_plan(
        operation, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5",
        operation == FlashOperation::Read ? std::nullopt
                                          : std::optional<bytes::Bytes>(bytes::Bytes(0x28000, 0)),
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01, 0x02, 0x03, 0x04}});
}

Result<FlashPlan> ecutek_plan(FlashOperation operation = FlashOperation::Read)
{
    return build_subaru_denso_mc68hc16y5_02_plan(
        operation, "sub_ecu_denso_mc68hc16y5_02_ecutek", "MC68HC16Y5",
        operation == FlashOperation::Read ? std::nullopt
                                          : std::optional<bytes::Bytes>(bytes::Bytes(0x28000, 0)),
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01, 0x02, 0x03, 0x04}});
}

Result<FlashPlan> tpu_read_plan()
{
    return build_subaru_denso_mc68hc16y5_02_plan(
        FlashOperation::Read, "sub_ecu_denso_mc68hc16y5_02_tpu", "MC68HC16Y5_TPU",
        std::nullopt,
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01, 0x02, 0x03, 0x04}});
}

bytes::Bytes framed(std::uint8_t opcode, bytes::ByteView extra = {})
{
    const std::uint16_t datalen_plus_one = static_cast<std::uint16_t>(extra.size() + 1);
    return composeBeWithChecksum(bytes::sum8, std::uint16_t{0xBEEF}, datalen_plus_one,
                                 bytes::Byte(opcode), extra);
}

bytes::Bytes stock_upload_request()
{
    return {
        0x53,
        0x02,
        0x00,
        0x00,
        0x00,
        0x10,
        0x64,
        0x67,
        0x39,
        0x41,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x9A,
    };
}

void script_stock_connect_and_upload(ScriptedKlineFlashTransport& transport)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:69-70,
    // 111-146, 220-281, and 1139-1168.
    transport.queue_no_frame();
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x4D, 0x00, 0xB3});
    transport.expectWrite(stock_upload_request());
    // Legacy upload_kernel():270-284 treats a real read timeout/no frame as
    // the success sentinel. Preserve OptionalBytes' distinction from a
    // present, zero-length frame in this end-to-end fixture.
    transport.queue_no_frame();
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K', 'I', 'D'}));
}

void script_read_page(ScriptedKlineFlashTransport& transport, std::uint32_t address,
                      bytes::Byte fill, std::uint8_t response_opcode = 0x43)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:323-455.
    transport.expectWrite(
        framed(0x03, composeBe(0x00_b, u24(address), std::uint16_t{0x400})));
    transport.queueRead(framed(response_opcode, bytes::Bytes(0x400, fill)));
}

bytes::Bytes u32_be(std::uint32_t value)
{
    return composeBe(value);
}

std::size_t packed_block_offset(const flashdev_t& device, unsigned block_no)
{
    std::size_t offset = 0;
    for (unsigned index = 0; index < block_no; ++index)
    {
        offset += device.fblocks[index].len;
    }
    return offset;
}

void script_crc_compare(ScriptedKlineFlashTransport& transport, const flashdev_t& device,
                        bytes::ByteView image, std::optional<unsigned> differing_block)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:591-715.
    std::size_t image_offset = 0;
    for (unsigned block_no = 0; block_no < device.numblocks; ++block_no)
    {
        const auto& block = device.fblocks[block_no];
        const bytes::Bytes request_payload = composeBe(block.start, 0x00_b, u24(block.len));
        transport.expectWrite(framed(0x02, request_payload));

        std::uint32_t ecu_crc =
            fastecu::checksum::crc32(image.data() + image_offset, block.len);
        if (differing_block == block_no)
        {
            ecu_crc ^= 0x00000001u;
        }
        transport.queueRead(framed(0x42, u32_be(ecu_crc)));
        transport.queue_no_frame();
        image_offset += block.len;
    }
}

void script_flash_init(ScriptedKlineFlashTransport& transport, bool test_write)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:717-838.
    transport.expectWrite(framed(0x05));
    transport.queueRead(framed(0x45, bytes::Bytes{0x00, 0x00, 0x02, 0x06}));
    transport.expectWrite(framed(0x06));
    transport.queueRead(framed(0x46, bytes::Bytes{0x00, 0x00, 0x10, 0x00}));
    const std::uint8_t enable_opcode = test_write ? 0x21 : 0x20;
    transport.expectWrite(framed(enable_opcode));
    transport.queueRead(framed(static_cast<std::uint8_t>(enable_opcode | 0x40)));
}

void script_prog_volt(ScriptedKlineFlashTransport& transport)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:881-921.
    transport.expectWrite(framed(0x04));
    transport.queueRead(framed(0x44, bytes::Bytes{0x04, 0xB0}));
}

void script_block_transfer(ScriptedKlineFlashTransport& transport, const flashdev_t& device,
                           bytes::ByteView image, unsigned block_no, bool test_write)
{
    constexpr std::uint32_t kChunkSize = 0x200;
    constexpr std::uint32_t kCommitSize = 0x1000;
    const auto& block = device.fblocks[block_no];
    const std::size_t block_image_offset = packed_block_offset(device, block_no);

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:950-992.
    if (!test_write)
    {
        transport.expectWrite(framed(0x25, u32_be(block.start)));
        transport.queueRead(framed(0x65));
    }

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:994-1128.
    for (std::uint32_t offset = 0; offset < block.len; offset += kChunkSize)
    {
        const std::uint32_t address = block.start + offset;
        bytes::Bytes write_payload = u32_be(address);
        write_payload.insert(write_payload.end(), image.begin() + block_image_offset + offset,
                             image.begin() + block_image_offset + offset + kChunkSize);
        transport.expectWrite(framed(0x22, write_payload));
        transport.queueRead(framed(0x62));

        if ((offset + kChunkSize) % kCommitSize == 0)
        {
            const std::uint32_t commit_offset = offset + kChunkSize - kCommitSize;
            const std::uint32_t commit_address = block.start + commit_offset;
            const std::uint32_t commit_crc = fastecu::checksum::crc32(
                image.data() + block_image_offset + commit_offset, kCommitSize);
            bytes::Bytes commit_payload = u32_be(commit_address);
            commit_payload.push_back(0x10);
            commit_payload.push_back(0x00);
            bytes::Bytes crc = u32_be(commit_crc);
            commit_payload.insert(commit_payload.end(), crc.begin(), crc.end());
            const std::uint8_t commit_opcode = test_write ? 0x23 : 0x24;
            transport.expectWrite(framed(commit_opcode, commit_payload));
            transport.queueRead(framed(static_cast<std::uint8_t>(commit_opcode | 0x40)));
        }
    }
    transport.queue_no_frame();
}

Result<FlashPlan> stock_write_plan(FlashOperation operation, bytes::Bytes image)
{
    return build_subaru_denso_mc68hc16y5_02_plan(
        operation, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5", std::move(image),
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01, 0x02, 0x03, 0x04}});
}

bytes::Bytes write_chunk_request(const flashdev_t& device, bytes::ByteView image,
                                 unsigned block_no, std::uint32_t offset)
{
    constexpr std::uint32_t kChunkSize = 0x200;
    const auto& block = device.fblocks[block_no];
    bytes::Bytes payload = u32_be(block.start + offset);
    const std::size_t image_offset = packed_block_offset(device, block_no) + offset;
    payload.insert(payload.end(), image.begin() + image_offset,
                   image.begin() + image_offset + kChunkSize);
    return framed(0x22, payload);
}

void script_write_prefix(ScriptedKlineFlashTransport& transport, const flashdev_t& device,
                         bytes::ByteView image, unsigned block_no, bool test_write)
{
    script_stock_connect_and_upload(transport);
    script_crc_compare(transport, device, image, block_no);
    script_flash_init(transport, test_write);
    script_prog_volt(transport);
    if (!test_write)
    {
        transport.expectWrite(framed(0x25, u32_be(device.fblocks[block_no].start)));
        transport.queueRead(framed(0x65));
    }
}

class CancelAfterFirstPageClock final : public FakeClock
{
  public:
    explicit CancelAfterFirstPageClock(ToggleCancellation& cancellation) : cancellation_(cancellation)
    {
    }

    Status sleep(int ms, const ICancellationToken& cancellation) override
    {
        Status result = FakeClock::sleep(ms, cancellation);
        if (result.has_value() && ms == 1 && !cancelled_after_page_)
        {
            cancelled_after_page_ = true;
            cancellation_.cancel();
        }
        return result;
    }

  private:
    ToggleCancellation& cancellation_;
    bool cancelled_after_page_ = false;
};

config::ConfigPaths eeprom_paths()
{
    return {.kernel_files_directory = "kernels/", .protocols_file = "protocols.cfg"};
}

Result<FlashPlan> different_family_plan()
{
    constexpr char kConfig[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <protocols>
    <protocol name="sub_ecu_eeprom_denso_sh7055_kline" alias="SH7055 EEPROM K-Line">
      <ecu>Denso SH7055</ecu><mcu>SH7055</mcu><kernel>kernel.bin</kernel><kernel_addr>0xFFFF6004</kernel_addr>
    </protocol>
  </protocols>
  <car_models>
    <car_model><make>Subaru</make><model>Impreza</model><version>WRX</version><protocol>sub_ecu_eeprom_denso_sh7055_kline</protocol></car_model>
  </car_models>
</config>)";
    InMemoryFileRepository files;
    files.files["protocols.cfg"] = std::vector<std::uint8_t>(kConfig, kConfig + sizeof(kConfig) - 1);
    files.files["kernels/kernel.bin"] = {0x01, 0x02, 0x03, 0x04};
    return build_eeprom_read_plan(eeprom_paths(), "sub_ecu_eeprom_denso_sh7055_kline",
                                  EepromReadMode::Mode2, files);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WrongFamilyPlanFails)
{
    auto plan = different_family_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

TEST(SubaruDensoMc68hc16y5_02Executor, MalformedFamilyPlanFailsBeforeAnyIo)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    auto plan = validate_and_build(FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruDensoMc68hc16y5_02,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_denso_mc68hc16y5_02",
        .mcu_name = "MC68HC16Y5",
        .transfer_region = {device->fblocks[0].start, device->romsize},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01}},
        .family_plan = SubaruDensoMc68hc16y5_02Plan{
            .connect_baud = 12345,
            .kernel_baud = 9600,
            .encryption_xor = 0x55,
            .kernel_magic = 0x3941,
            .bootloader_ok = {0x4d, 0x00, 0xb3},
        },
    });
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_FALSE(transport.last_config_.has_value());
    EXPECT_TRUE(transport.read_timeouts_.empty());
    EXPECT_TRUE(transport.control_line_trace_.empty());
    EXPECT_EQ(transport.close_call_count_, 0);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ConnectsViaWrx02InitAndUploadsPaddedKernel)
{
    auto plan = stock_plan(FlashOperation::Write);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:69-70.
    transport.queue_no_frame();
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-146.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x4D, 0x00, 0xB3});

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:220-281.
    // The four input bytes are padded to 16 before encryption; magic patches
    // encrypted offsets 2..3, and each trailing zero encrypts to 0x65.
    const bytes::Bytes upload{
        0x53,
        0x02,
        0x00,
        0x00,
        0x00,
        0x10,
        0x64,
        0x67,
        0x39,
        0x41,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x9A,
    };
    transport.expectWrite(upload);
    transport.queue_no_frame();

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K', 'I', 'D'}));
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    script_crc_compare(transport, *device, *plan->image(), std::nullopt);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.control_line_trace_,
              (std::vector<ScriptedKlineFlashTransport::ControlLineAction>{
                  ScriptedKlineFlashTransport::ControlLineAction::DisableLecLines,
                  ScriptedKlineFlashTransport::ControlLineAction::PulseLec2,
                  ScriptedKlineFlashTransport::ControlLineAction::EnableProgrammingVoltageLine,
              }));
    EXPECT_EQ(transport.operation_trace_.front(),
              ScriptedKlineFlashTransport::Operation::DisableLecLines);
    EXPECT_EQ(transport.operation_trace_.at(1),
              ScriptedKlineFlashTransport::Operation::Read10);
    EXPECT_EQ(transport.lec_2_pulse_timeouts_, (std::vector<int>{200}));
    EXPECT_EQ(std::count(transport.read_timeouts_.begin(), transport.read_timeouts_.end(), 200), 12);
    EXPECT_EQ(transport.close_call_count_, 1);
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-119,
    // 289-293, and 1139-1162: 200 + 200 + 50 + 1500 + 200 ms.
    EXPECT_EQ(clock.now_, 2150u);
}

TEST(SubaruDensoMc68hc16y5_02Executor, PresentEmptyUploadFrameIsNotNoFrameSuccess)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    transport.queue_no_frame();
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x4D, 0x00, 0xB3});
    transport.expectWrite(stock_upload_request());
    transport.queueRead(bytes::Bytes{});

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ConnectFallsBackToKernelAlivePoll)
{
    auto plan = stock_plan(FlashOperation::Write);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:69-70.
    transport.queueRead(bytes::Bytes{0xDE, 0xAD}); // stale bytes are discarded
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-179.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x00});
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K'}));
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    script_crc_compare(transport, *device, *plan->image(), std::nullopt);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.control_line_trace_,
              (std::vector<ScriptedKlineFlashTransport::ControlLineAction>{
                  ScriptedKlineFlashTransport::ControlLineAction::DisableLecLines,
                  ScriptedKlineFlashTransport::ControlLineAction::PulseLec2,
                  ScriptedKlineFlashTransport::ControlLineAction::DisableLecLines,
                  ScriptedKlineFlashTransport::ControlLineAction::EnableProgrammingVoltageLine,
              }));
    EXPECT_EQ(transport.close_call_count_, 1);
    EXPECT_EQ(std::count(transport.read_timeouts_.begin(), transport.read_timeouts_.end(), 200), 11);
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-119,
    // 149-155, and 1139-1162: 200 + 200 + 50 + 100 + 200 ms.
    EXPECT_EQ(clock.now_, 750u);
}

TEST(SubaruDensoMc68hc16y5_02Executor, NoFrameBootInitFallsBackToKernelAlivePoll)
{
    auto plan = stock_plan(FlashOperation::Write);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    transport.queue_no_frame(); // legacy operation.cpp:69-70 initial drain
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    // Legacy connect_bootloader():119-155: an empty read is a bad/missing init
    // response and falls through to the 62500-baud kernel-ID probe.
    transport.queue_no_frame();
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K'}));
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    script_crc_compare(transport, *device, *plan->image(), std::nullopt);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{62500}));
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, EcutekUsesItsDistinctBootloaderAndKernelWireValues)
{
    auto plan = ecutek_plan(FlashOperation::Write);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.queue_no_frame(); // legacy operation.cpp:69-70 initial drain
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:115-146.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x4C, 0x00, 0xB4});
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:204-281.
    transport.expectWrite(bytes::Bytes{
        0x53,
        0x02,
        0x00,
        0x00,
        0x00,
        0x10,
        0x60,
        0x63,
        0x39,
        0x40,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0xD3,
    });
    transport.queue_no_frame();
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K'}));
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    script_crc_compare(transport, *device, *plan->image(), std::nullopt);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{11700, 62500}));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, ConnectFailsWithNoValidResponseAtAll)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.queue_no_frame(); // legacy operation.cpp:69-70 initial drain
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-179.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x00});
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queue_no_frame();

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, CancellationBeforeConnectStopsImmediately)
{
    class AlreadyCancelled : public ICancellationToken
    {
      public:
        bool cancelled() const override
        {
            return true;
        }
    } cancellation;
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ShortKlineWriteFailsBeforeRead)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ShortWriteTransport transport;
    transport.queue_no_frame(); // legacy operation.cpp:69-70 initial drain
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST(SubaruDensoMc68hc16y5_02Executor, InitialDrainTransportErrorStopsBeforeBootloaderTraffic)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.queue_error(ErrorKind::Disconnected, "drain failed");
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, CancellationAtInitialDrainStopsBeforeBootloaderWrite)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ToggleCancellation cancellation;
    DrainCancellingTransport transport(cancellation);
    FakeClock clock;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.write_attempts_.empty());
    EXPECT_EQ(transport.read_timeouts_, (std::vector<int>{10}));
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, CloseOnlyFailureIsReturnedAfterSuccessfulPhases)
{
    auto plan = stock_plan(FlashOperation::Write);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.queue_no_frame(); // legacy operation.cpp:69-70 initial drain
    transport.close_result_ = fail(ErrorKind::Internal, "close failed");
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-179.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x00});
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K'}));
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    script_crc_compare(transport, *device, *plan->image(), std::nullopt);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, PhaseFailureWinsOverCloseFailureAndLogsCleanupFailure)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.queue_no_frame(); // legacy operation.cpp:69-70 initial drain
    transport.close_result_ = fail(ErrorKind::Internal, "close failed");
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-179.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x00});
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queue_no_frame();

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(transport.close_call_count_, 1);
    ASSERT_FALSE(events.logs.empty());
    EXPECT_EQ(events.logs.back(),
              (std::pair<LogLevel, std::string>{
                  LogLevel::Warning, "close failed after MC68HC16Y5_02 phase error"}));
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadReturnsAssembledPageBytes)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);

    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes expected;
    std::size_t logical_page = 0;
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        for (std::uint32_t offset = 0; offset < block.len; offset += 0x400)
        {
            const auto fill = static_cast<bytes::Byte>(logical_page++);
            script_read_page(transport, block.start + offset, fill);
            expected.insert(expected.end(), 0x400, fill);
        }
    }

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->operation, FlashOperation::Read);
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, expected);
    ASSERT_EQ(result->read_bytes->size(), 0x28000u);
    // Packed output joins the final byte before the physical RAM hole to the
    // first byte read at wire address 0x28000; the 0x8000-byte hole is absent.
    EXPECT_EQ(result->read_bytes->at(0x1FFFF), 0x7Fu);
    EXPECT_EQ(result->read_bytes->at(0x20000), 0x80u);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, TpuReadHonorsDeclaredPackedRomSize)
{
    auto plan = tpu_read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    const flashdev_t *device = find_flash_device("MC68HC16Y5_TPU");
    ASSERT_NE(device, nullptr);
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    for (std::uint32_t offset = 0; offset < device->romsize; offset += 0x400)
    {
        script_read_page(transport, device->fblocks[0].start + offset, 0x6a);
    }
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), device->romsize);
    EXPECT_TRUE(std::all_of(result->read_bytes->begin(), result->read_bytes->end(),
                            [](bytes::Byte value)
                            { return value == 0x6a; }));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadRejectsMalformedPageResponse)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    script_read_page(transport, 0x00000000, 0xA5, 0x44);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadRejectsTruncatedValidMarkerResponse)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:408-413.
    transport.expectWrite(framed(0x03, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x04, 0x00}));
    transport.queueRead(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x43});

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadRejectsShortPageResponse)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    transport.expectWrite(
        framed(0x03, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x04, 0x00}));
    // Valid BEEF/0x43 envelope but one byte less than the requested page.
    transport.queueRead(framed(0x43, bytes::Bytes(0x3FF, 0xA5)));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadCancelsBetweenPages)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    script_read_page(transport, 0x00000000, 0xA5);

    ToggleCancellation cancellation;
    CancelAfterFirstPageClock clock(cancellation);
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 4u);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteSkipsWhenNoBlockDiffers)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, std::nullopt);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 3u + device->numblocks);
    EXPECT_EQ(transport.programming_voltage_line_write_index_, 3u + device->numblocks);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteReflashesOnlyDifferingBlocks)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0x00);
    const std::size_t image_offset = packed_block_offset(*device, kDifferingBlock);
    for (std::size_t offset = 0; offset < device->fblocks[kDifferingBlock].len; ++offset)
    {
        image[image_offset + offset] = static_cast<bytes::Byte>((offset * 17u + 3u) & 0xFF);
    }
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kDifferingBlock);
    script_flash_init(transport, false);
    script_prog_volt(transport);
    script_block_transfer(transport, *device, image, kDifferingBlock, false);
    script_crc_compare(transport, *device, image, std::nullopt);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.control_line_trace_.back(),
              ScriptedKlineFlashTransport::ControlLineAction::EnableProgrammingVoltageLine);
    EXPECT_EQ(clock.now_, 4050u);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, TestWriteSendsValidateNotCommit)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0x00);
    const std::size_t image_offset = packed_block_offset(*device, kDifferingBlock);
    std::fill(image.begin() + image_offset,
              image.begin() + image_offset + device->fblocks[kDifferingBlock].len, 0xA5);
    auto plan = stock_write_plan(FlashOperation::TestWrite, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kDifferingBlock);
    script_flash_init(transport, true);
    script_prog_volt(transport);
    script_block_transfer(transport, *device, image, kDifferingBlock, true);
    script_crc_compare(transport, *device, image, kDifferingBlock);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::TestWrite);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteFailsOnRejectedEraseResponse)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kDifferingBlock);
    script_flash_init(transport, false);
    script_prog_volt(transport);
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:950-987.
    transport.expectWrite(framed(0x25, u32_be(device->fblocks[kDifferingBlock].start)));
    transport.queueRead(framed(0x64));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteCancelsMidBlockTransfer)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ToggleCancellation cancellation;
    CancelAfterEraseTransport transport(cancellation);
    script_stock_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kDifferingBlock);
    script_flash_init(transport, false);
    script_prog_volt(transport);
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:950-1000.
    transport.expectWrite(framed(0x25, u32_be(device->fblocks[kDifferingBlock].start)));
    transport.queueRead(framed(0x65));

    FakeClock clock;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.flash_buffer_write_attempts_, 0u);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteAcceptsFragmentedBlockCrcAndDrainsIt)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    std::size_t image_offset = 0;
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        bytes::Bytes request_payload = u32_be(block.start);
        request_payload.push_back(0x00);
        request_payload.push_back(0x00);
        request_payload.push_back(0x40);
        request_payload.push_back(0x00);
        transport.expectWrite(framed(0x02, request_payload));
        const std::uint32_t crc =
            fastecu::checksum::crc32(image.data() + image_offset, block.len);
        bytes::Bytes response = framed(0x42, u32_be(crc));
        if (block_no == 0)
        {
            transport.queueRead(bytes::ByteView(response).first(6));
            transport.queueRead(bytes::ByteView(response).subspan(6));
        }
        else
        {
            transport.queueRead(response);
        }
        transport.queue_no_frame();
        image_offset += block.len;
    }

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts_.begin(), transport.read_timeouts_.end(), 50), 1);
    EXPECT_EQ(clock.now_, 2250u);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteAcceptsBlockCrcAfterEmptyInitialRead)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    std::size_t image_offset = 0;
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        bytes::Bytes request_payload = u32_be(block.start);
        request_payload.insert(request_payload.end(), {0x00, 0x00, 0x40, 0x00});
        transport.expectWrite(framed(0x02, request_payload));
        const std::uint32_t crc =
            fastecu::checksum::crc32(image.data() + image_offset, block.len);
        if (block_no == 0)
        {
            transport.queue_no_frame();
            transport.queueRead(framed(0x42, u32_be(crc)));
        }
        else
        {
            transport.queueRead(framed(0x42, u32_be(crc)));
        }
        transport.queue_no_frame();
        image_offset += block.len;
    }

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts_.begin(), transport.read_timeouts_.end(), 50), 1);
    EXPECT_EQ(clock.now_, 2250u);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteRejectsTruncatedBlockCrcAfterBoundedReads)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    transport.expectWrite(framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00,
                                                    0x00, 0x00, 0x40, 0x00}));
    transport.queueRead(bytes::Bytes{0xBE, 0xEF, 0x00, 0x05, 0x42});
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        transport.queue_no_frame();
    }

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts_.begin(), transport.read_timeouts_.end(), 50), 20);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteRejectsNegativeBlockCrcResponse)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    transport.expectWrite(framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00,
                                                    0x00, 0x00, 0x40, 0x00}));
    transport.queueRead(framed(0x7F, bytes::Bytes{0x00, 0x00, 0x00, 0x00}));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, WritePropagatesBlockCrcDrainError)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    transport.expectWrite(framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00,
                                                    0x00, 0x00, 0x40, 0x00}));
    const std::uint32_t crc = fastecu::checksum::crc32(image.data(), 0x4000);
    transport.queueRead(framed(0x42, u32_be(crc)));
    transport.queue_error(ErrorKind::Disconnected, "CRC drain failed");

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(result.error().detail, "CRC drain failed");
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteFailsOnRejectedFlashBufferResponse)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_prefix(transport, *device, image, kBlock, false);
    transport.expectWrite(write_chunk_request(*device, image, kBlock, 0));
    transport.queueRead(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x62});

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteFailsOnRejectedCommitResponse)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_prefix(transport, *device, image, kBlock, false);
    for (std::uint32_t offset = 0; offset < 0x1000; offset += 0x200)
    {
        transport.expectWrite(write_chunk_request(*device, image, kBlock, offset));
        transport.queueRead(framed(0x62));
    }
    const std::uint32_t start = device->fblocks[kBlock].start;
    const std::uint32_t crc = fastecu::checksum::crc32(
        image.data() + packed_block_offset(*device, kBlock), 0x1000);
    bytes::Bytes commit_payload = u32_be(start);
    commit_payload.insert(commit_payload.end(), {0x10, 0x00});
    bytes::Bytes crc_bytes = u32_be(crc);
    commit_payload.insert(commit_payload.end(), crc_bytes.begin(), crc_bytes.end());
    transport.expectWrite(framed(0x24, commit_payload));
    transport.queueRead(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x64});

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, TestWriteFailsOnRejectedValidateResponse)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::TestWrite, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_prefix(transport, *device, image, kBlock, true);
    for (std::uint32_t offset = 0; offset < 0x1000; offset += 0x200)
    {
        transport.expectWrite(write_chunk_request(*device, image, kBlock, offset));
        transport.queueRead(framed(0x62));
    }
    const std::uint32_t start = device->fblocks[kBlock].start;
    const std::uint32_t crc = fastecu::checksum::crc32(
        image.data() + packed_block_offset(*device, kBlock), 0x1000);
    bytes::Bytes validate_payload = u32_be(start);
    validate_payload.insert(validate_payload.end(), {0x10, 0x00});
    bytes::Bytes crc_bytes = u32_be(crc);
    validate_payload.insert(validate_payload.end(), crc_bytes.begin(), crc_bytes.end());
    transport.expectWrite(framed(0x23, validate_payload));
    transport.queueRead(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x63});

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteLogsRemainingMismatchAfterVerification)
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = stock_write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kBlock);
    script_flash_init(transport, false);
    script_prog_volt(transport);
    script_block_transfer(transport, *device, image, kBlock, false);
    script_crc_compare(transport, *device, image, kBlock);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(std::find(events.logs.begin(), events.logs.end(),
                          std::pair{LogLevel::Error,
                                    std::string{"Flash verification differs; do not power off, the kernel is still running"}}) !=
                events.logs.end());
}

} // namespace
} // namespace fastecu::flash
