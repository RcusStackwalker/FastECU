#include "src/backend/flash/ecu/subaru_denso_sh7055_02_executor.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash
{
namespace
{

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
        return cancelled_;
    }

    void cancel()
    {
        cancelled_ = true;
    }

  private:
    bool cancelled_ = false;
};

class FlipAfter final : public ICancellationToken
{
  public:
    explicit FlipAfter(int allowed_checks) : allowed_checks_(allowed_checks)
    {
    }

    bool cancelled() const override
    {
        return checks_++ >= allowed_checks_;
    }

  private:
    int allowed_checks_;
    mutable int checks_ = 0;
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

class RecordingClock final : public FakeClock
{
  public:
    Status sleep(int ms, const ICancellationToken& cancellation) override
    {
        sleep_calls.push_back(ms);
        return FakeClock::sleep(ms, cancellation);
    }

    std::vector<int> sleep_calls;
};

class CancelAtPostUploadDelayClock final : public FakeClock
{
  public:
    Status sleep(int ms, const ICancellationToken& cancellation) override
    {
        sleep_calls.push_back(ms);
        if (ms == 5000)
        {
            return fail(ErrorKind::Cancelled, "cancelled during OpenPort2 upload delay");
        }
        return FakeClock::sleep(ms, cancellation);
    }

    std::vector<int> sleep_calls;
};

Result<FlashPlan> read_plan(bytes::Bytes kernel_bytes = {0x01, 0x02, 0x03, 0x04, 0x05})
{
    return build_subaru_denso_sh7055_02_plan(
        FlashOperation::Read, "sub_ecu_denso_sh7055_02", "SH7055", std::nullopt,
        KernelImage{.id = "k", .load_address = 0xFFFF6004, .bytes = std::move(kernel_bytes)});
}

Result<FlashPlan> write_plan(FlashOperation operation = FlashOperation::Write,
                             bytes::Bytes image = {})
{
    const int index = find_flash_device_index("SH7055");
    if (index < 0)
    {
        return fail(ErrorKind::InvalidConfig, "SH7055 fixture is missing");
    }
    if (image.empty())
    {
        image.resize(flashdevices[index].romsize, bytes::Byte{0});
    }
    return build_subaru_denso_sh7055_02_plan(
        operation, "sub_ecu_denso_sh7055_02", "SH7055", std::move(image),
        KernelImage{.id = "k",
                    .load_address = 0xFFFF6004,
                    .bytes = {0x01, 0x02, 0x03, 0x04, 0x05}});
}

Result<FlashPlan> malformed_plan(std::vector<ConfirmationSpec> confirmations,
                                 std::uint8_t tester_id = 0xF0)
{
    const int index = find_flash_device_index("SH7055");
    if (index < 0)
    {
        return fail(ErrorKind::InvalidConfig, "SH7055 fixture is missing");
    }
    return validate_and_build(FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruDensoSh7055_02,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_denso_sh7055_02",
        .mcu_name = "SH7055",
        .transfer_region = {flashdevices[index].fblocks[0].start, flashdevices[index].romsize},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0xFFFF6004, .bytes = {0x01}},
        .family_plan = SubaruDensoSh7055_02Plan{
            .tester_id = tester_id,
            .target_id = 0x10,
            .read_ecu_id = true,
        },
        .confirmations = std::move(confirmations),
    });
}

bytes::Bytes framed(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes out{0xBE, 0xEF};
    const std::uint16_t length = static_cast<std::uint16_t>(payload.size() + 1);
    out.push_back(static_cast<bytes::Byte>((length >> 8) & 0xFF));
    out.push_back(static_cast<bytes::Byte>(length & 0xFF));
    out.push_back(opcode);
    out.insert(out.end(), payload.begin(), payload.end());
    out.push_back(bytes::sum8(out));
    return out;
}

bytes::Bytes ecu_id_response()
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:133-168:
    // byte 4 is the positive-response marker and the five ECU-ID bytes are
    // sliced from offsets 8..12. The expected hexadecimal ID is 4142434445.
    return {0x80, 0x10, 0xF0, 0x09, 0xFF, 0x00, 0x00, 0x00,
            0x41, 0x42, 0x43, 0x44, 0x45, 0x00};
}

bytes::Bytes exact_upload_request()
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:267-295.
    // Five input bytes are padded to eight. The inner checksum is 0x4B at
    // offset 7 and the checksum over the complete message is the final 0xDF.
    return {0x53, 0xFF, 0x60, 0x00, 0x00, 0x0C, 0x65, 0x4B, 0x31, 0x61,
            0x64, 0x67, 0x66, 0x61, 0x60, 0x65, 0x65, 0x65, 0xDF};
}

bytes::Bytes u32_be(std::uint32_t value)
{
    return {
        static_cast<bytes::Byte>((value >> 24) & 0xFF),
        static_cast<bytes::Byte>((value >> 16) & 0xFF),
        static_cast<bytes::Byte>((value >> 8) & 0xFF),
        static_cast<bytes::Byte>(value & 0xFF),
    };
}

bytes::Bytes crc_response(std::uint32_t crc)
{
    bytes::Bytes payload{0x05};
    bytes::Bytes crc_bytes = u32_be(crc);
    payload.insert(payload.end(), crc_bytes.begin(), crc_bytes.end());
    return framed(0x42, payload);
}

void script_read_page(ScriptedKlineFlashTransport& transport, std::uint32_t address,
                      bytes::Byte fill, std::uint8_t response_opcode = 0x43)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:360-430:
    // SUB_KERNEL_READ_AREA uses a zero byte plus the 24-bit address and a
    // fixed 0x400-byte page request. The reply carries the 0x43 acknowledgment.
    transport.expectWrite(framed(0x03, bytes::Bytes{
                                           0x00,
                                           static_cast<bytes::Byte>((address >> 16) & 0xFF),
                                           static_cast<bytes::Byte>((address >> 8) & 0xFF),
                                           static_cast<bytes::Byte>(address & 0xFF),
                                           0x04,
                                           0x00,
                                       }));
    transport.queueRead(framed(response_opcode, bytes::Bytes(0x400, fill)));
}

void script_failed_probe(ScriptedKlineFlashTransport& transport)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:108-131 and 1169-1198.
    transport.expectWrite(framed(0x01));
    transport.queue_no_frame();
}

void script_wrx_preamble(ScriptedKlineFlashTransport& transport, bool read_ecu_id)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:69-70.
    transport.queue_no_frame();
    script_failed_probe(transport);
    if (read_ecu_id)
    {
        // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:133-168 and 1206-1218.
        transport.expectWrite(bytes::Bytes{0x80, 0x10, 0xF0, 0x01, 0xBF, 0x40});
        transport.queueRead(ecu_id_response());
    }
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:170-172.
    transport.queue_no_frame();
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:194-196.
    transport.queue_no_frame();
}

void script_first_wrx_attempt_connects(ScriptedKlineFlashTransport& transport)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:198-221
    // and 1276-1292: check_received_message() returns zero only for an exact
    // three-byte match, so !check_received_message(...) means connected.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x4D, 0x00, 0xB3});
    transport.queue_no_frame();
}

void script_upload(ScriptedKlineFlashTransport& transport)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:267-328.
    transport.expectWrite(exact_upload_request());
    transport.queue_no_frame();
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K', 'I', 'D'}));
}

void script_write_connect_and_upload(ScriptedKlineFlashTransport& transport)
{
    script_wrx_preamble(transport, false);
    script_first_wrx_attempt_connects(transport);
    script_upload(transport);
}

void script_crc_compare(ScriptedKlineFlashTransport& transport, const flashdev_t& device,
                        bytes::ByteView image, std::optional<unsigned> differing_block)
{
    // Legacy check_romcrc(), lines 645-749: the CRC request uses a 32-bit
    // address, a zero prefix, and a 24-bit block length. The response is
    // accumulated to ten bytes, unwrapped from its BEEF envelope, and drained.
    for (unsigned block_no = 0; block_no < device.numblocks; ++block_no)
    {
        const auto& block = device.fblocks[block_no];
        bytes::Bytes request_payload = u32_be(block.start);
        request_payload.push_back(0x00);
        request_payload.push_back(static_cast<bytes::Byte>((block.len >> 16) & 0xFF));
        request_payload.push_back(static_cast<bytes::Byte>((block.len >> 8) & 0xFF));
        request_payload.push_back(static_cast<bytes::Byte>(block.len & 0xFF));
        transport.expectWrite(framed(0x02, request_payload));

        std::uint32_t ecu_crc = fastecu::checksum::crc32(
            bytes::ByteView(image).subspan(block.start, block.len));
        if (differing_block == block_no)
        {
            ecu_crc ^= 0x00000001u;
        }
        transport.queueRead(crc_response(ecu_crc));
        transport.queue_no_frame();
    }
}

void script_flash_init(ScriptedKlineFlashTransport& transport, bool test_write)
{
    // Legacy init_flash_write(), lines 752-873. SH7055's 32-bit values are
    // at response offsets 6..9, so byte 5 is a deliberately non-zero prefix
    // that would expose accidental MC68-style 5..8 parsing.
    transport.expectWrite(framed(0x05));
    transport.queueRead(framed(0x45, bytes::Bytes{0xA5, 0x00, 0x00, 0x02, 0x06}));
    transport.expectWrite(framed(0x06));
    transport.queueRead(framed(0x46, bytes::Bytes{0x5A, 0x00, 0x00, 0x10, 0x00}));
    const std::uint8_t enable_opcode = test_write ? 0x21 : 0x20;
    transport.expectWrite(framed(enable_opcode));
    transport.queueRead(framed(static_cast<std::uint8_t>(enable_opcode | 0x40)));
}

void script_prog_volt(ScriptedKlineFlashTransport& transport)
{
    // Legacy reflash_block(), lines 914-944.
    transport.expectWrite(framed(0x04));
    transport.queueRead(framed(0x44, bytes::Bytes{0x04, 0xB0}));
}

bytes::Bytes write_chunk_request(const flashdev_t& device, bytes::ByteView image,
                                 unsigned block_no, std::uint32_t offset)
{
    constexpr std::uint32_t kChunkSize = 0x200;
    const auto& block = device.fblocks[block_no];
    bytes::Bytes payload = u32_be(block.start + offset);
    payload.insert(payload.end(), image.begin() + block.start + offset,
                   image.begin() + block.start + offset + kChunkSize);
    return framed(0x22, payload);
}

bytes::Bytes commit_request(const flashdev_t& device, bytes::ByteView image,
                            unsigned block_no, std::uint32_t offset, bool test_write)
{
    constexpr std::uint32_t kCommitSize = 0x1000;
    const auto& block = device.fblocks[block_no];
    const std::uint32_t address = block.start + offset;
    const std::uint32_t crc = fastecu::checksum::crc32(
        bytes::ByteView(image).subspan(address, kCommitSize));
    bytes::Bytes payload = u32_be(address);
    payload.insert(payload.end(), {0x10, 0x00});
    bytes::Bytes crc_bytes = u32_be(crc);
    payload.insert(payload.end(), crc_bytes.begin(), crc_bytes.end());
    return framed(test_write ? 0x23 : 0x24, payload);
}

void script_block_transfer(ScriptedKlineFlashTransport& transport, const flashdev_t& device,
                           bytes::ByteView image, unsigned block_no, bool test_write)
{
    constexpr std::uint32_t kChunkSize = 0x200;
    constexpr std::uint32_t kCommitSize = 0x1000;
    const auto& block = device.fblocks[block_no];
    if (!test_write)
    {
        transport.expectWrite(framed(0x25, u32_be(block.start)));
        transport.queueRead(framed(0x65));
    }
    for (std::uint32_t offset = 0; offset < block.len; offset += kChunkSize)
    {
        transport.expectWrite(write_chunk_request(device, image, block_no, offset));
        transport.queueRead(framed(0x62));
        if ((offset + kChunkSize) % kCommitSize == 0)
        {
            const std::uint32_t commit_offset = offset + kChunkSize - kCommitSize;
            const std::uint8_t opcode = test_write ? 0x23 : 0x24;
            transport.expectWrite(
                commit_request(device, image, block_no, commit_offset, test_write));
            transport.queueRead(framed(static_cast<std::uint8_t>(opcode | 0x40)));
        }
    }
    transport.queue_no_frame();
}

void script_write_prefix(ScriptedKlineFlashTransport& transport, const flashdev_t& device,
                         bytes::ByteView image, unsigned block_no, bool test_write)
{
    script_write_connect_and_upload(transport);
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

bool has_log(const RecordingEventSink& events, std::string_view message)
{
    return std::ranges::any_of(events.logs, [message](const auto& entry)
                               { return entry.second == message; });
}

TEST(SubaruDensoSh7055_02Executor, KernelAlreadyAliveSkipsWrxInitEcuIdAndUpload)
{
    auto plan = write_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    ScriptedKlineFlashTransport transport;
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:69-70.
    transport.queue_no_frame();
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:108-126 and 1169-1198.
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K'}));
    script_crc_compare(transport, *device, *plan->image(), std::nullopt);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
    ASSERT_TRUE(transport.last_config_.has_value());
    EXPECT_EQ(transport.last_config_->baud, 62500);
    EXPECT_EQ(transport.last_config_->tester_id, 0xF0);
    EXPECT_EQ(transport.last_config_->target_id, 0x10);
}

TEST(SubaruDensoSh7055_02Executor, KernelAliveReadReturnsNoRomId)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    ScriptedKlineFlashTransport transport;
    transport.queue_no_frame();
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K'}));
    for (std::uint32_t offset = 0; offset < device->romsize; offset += 0x400)
    {
        script_read_page(transport, device->fblocks[0].start + offset, 0x5a);
    }

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), device->romsize);
    EXPECT_FALSE(result->rom_id.has_value());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(transport.baud_calls_.empty());
}

TEST(SubaruDensoSh7055_02Executor, RejectsMissingConfirmationAndMalformedFamilyBeforeTransportIo)
{
    for (auto plan : {
             malformed_plan({}),
             malformed_plan({ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition}}, 0xF1),
         })
    {
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        ScriptedKlineFlashTransport transport;
        FakeClock clock;
        NeverCancelled cancellation;
        RecordingEventSink events;
        SubaruDensoSh7055_02Executor executor;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
        EXPECT_FALSE(transport.last_config_.has_value());
        EXPECT_EQ(transport.writesConsumed(), 0u);
        EXPECT_TRUE(transport.read_timeouts_.empty());
        EXPECT_TRUE(transport.control_line_trace_.empty());
        EXPECT_EQ(transport.close_call_count_, 0);
    }
}

TEST(SubaruDensoSh7055_02Executor, ReadSurfacesEcuIdInResult)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    transport.post_kernel_upload_delay_required_ = true;
    script_wrx_preamble(transport, true);
    script_first_wrx_attempt_connects(transport);
    script_upload(transport);
    const int device_index = find_flash_device_index("SH7055");
    ASSERT_GE(device_index, 0);
    const auto& device = flashdevices[device_index];
    for (std::uint32_t offset = 0; offset < device.romsize; offset += 0x400)
    {
        script_read_page(transport, device.fblocks[0].start + offset, 0x5A);
    }

    RecordingClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->rom_id, std::optional<std::string>{"4142434445"});
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(has_log(events, "ECU ID: 4142434445"));
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{4800, 9600, 62500}));
    EXPECT_EQ(transport.control_line_trace_,
              (std::vector<ScriptedKlineFlashTransport::ControlLineAction>{
                  ScriptedKlineFlashTransport::ControlLineAction::DisableLecLines,
                  ScriptedKlineFlashTransport::ControlLineAction::DisableLecLines,
                  ScriptedKlineFlashTransport::ControlLineAction::PulseLec2,
              }));
    EXPECT_EQ(transport.lec_2_pulse_timeouts_, (std::vector<int>{200}));
    std::vector<int> expected_sleeps{200, 1000, 1000, 1000, 250, 190, 100, 5000, 100, 200};
    std::vector<int> expected_timeouts{10, 2000, 2000, 10, 10, 10, 10, 200, 2000};
    for (std::uint32_t offset = 0; offset < device.romsize; offset += 0x400)
    {
        expected_sleeps.insert(expected_sleeps.end(), {10, 1});
        expected_timeouts.push_back(3000);
    }
    EXPECT_EQ(clock.sleep_calls, expected_sleeps);
    EXPECT_EQ(transport.read_timeouts_, expected_timeouts);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, OpenPort2UploadDelayCancellationStopsBeforeResponseRead)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    transport.post_kernel_upload_delay_required_ = true;
    script_wrx_preamble(transport, true);
    script_first_wrx_attempt_connects(transport);
    transport.expectWrite(exact_upload_request());

    CancelAtPostUploadDelayClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(std::count(clock.sleep_calls.begin(), clock.sleep_calls.end(), 5000), 1);
    EXPECT_EQ(std::count(transport.read_timeouts_.begin(), transport.read_timeouts_.end(), 200), 0);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, ReadReturnsAssembledPageBytes)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_wrx_preamble(transport, true);
    script_first_wrx_attempt_connects(transport);
    script_upload(transport);
    const int device_index = find_flash_device_index("SH7055");
    ASSERT_GE(device_index, 0);
    const auto& device = flashdevices[device_index];

    bytes::Bytes expected;
    for (std::uint32_t offset = 0; offset < device.romsize; offset += 0x400)
    {
        const bytes::Byte fill = static_cast<bytes::Byte>(offset / 0x400);
        script_read_page(transport, device.fblocks[0].start + offset, fill);
        expected.insert(expected.end(), 0x400, fill);
    }

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, expected);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, ReadRejectsMalformedPageResponse)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_wrx_preamble(transport, true);
    script_first_wrx_attempt_connects(transport);
    script_upload(transport);
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:423-437:
    // the 0x43 acknowledgment is required before stripping the frame envelope.
    transport.expectWrite(framed(0x03, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x04, 0x00}));
    transport.queueRead(framed(0x44, bytes::Bytes(0x400, 0xA5)));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, ReadRejectsTruncatedPageResponse)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_wrx_preamble(transport, true);
    script_first_wrx_attempt_connects(transport);
    script_upload(transport);
    // The reply has the valid 0x43 envelope, but its payload is shorter than
    // the requested 0x400-byte legacy page (lines 415-416), so it must not
    // yield a silently undersized ROM.
    transport.expectWrite(framed(0x03, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x04, 0x00}));
    transport.queueRead(framed(0x43, bytes::Bytes{0xA5}));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, ReadCancelsBetweenPages)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_wrx_preamble(transport, true);
    script_first_wrx_attempt_connects(transport);
    script_upload(transport);
    script_read_page(transport, 0x00000000, 0xA5);

    ToggleCancellation cancellation;
    CancelAfterFirstPageClock clock(cancellation);
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 6u); // probe + SID BF + WRX + upload + kernel ID + first read
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, NoFrameWrxReplyRetriesUntilExactResponse)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_wrx_preamble(transport, true);
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:201-224
    // and 1276-1292: an empty response is not the exact three-byte success.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queue_no_frame();
    script_first_wrx_attempt_connects(transport);
    script_upload(transport);
    const int device_index = find_flash_device_index("SH7055");
    ASSERT_GE(device_index, 0);
    const auto& device = flashdevices[device_index];
    for (std::uint32_t offset = 0; offset < device.romsize; offset += 0x400)
    {
        script_read_page(transport, device.fblocks[0].start + offset, 0x5A);
    }

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 518u); // probe + SID BF + two WRX + upload + kernel ID + 512 reads
}

TEST(SubaruDensoSh7055_02Executor, WritePathSkipsEcuIdRead)
{
    auto plan = write_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    script_crc_compare(transport, *device, *plan->image(), std::nullopt);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_FALSE(has_log(events, "ECU ID: 4142434445"));
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{9600, 62500}));
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, WriteSkipsWhenNoBlockDiffers)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, std::nullopt);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 4u + device->numblocks);
    EXPECT_EQ(transport.programming_voltage_line_write_index_, 4u + device->numblocks);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, WriteReflashesOnlyDifferingBlocks)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0x00);
    const auto& block = device->fblocks[kDifferingBlock];
    for (std::size_t offset = 0; offset < block.len; ++offset)
    {
        image[block.start + offset] = static_cast<bytes::Byte>((offset * 17u + 3u) & 0xFF);
    }
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kDifferingBlock);
    script_flash_init(transport, false);
    script_prog_volt(transport);
    script_block_transfer(transport, *device, image, kDifferingBlock, false);
    script_crc_compare(transport, *device, image, std::nullopt);

    RecordingClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.control_line_trace_.back(),
              ScriptedKlineFlashTransport::ControlLineAction::EnableProgrammingVoltageLine);
    EXPECT_EQ(std::count(clock.sleep_calls.begin(), clock.sleep_calls.end(), 50),
              block.len / 0x200);
    EXPECT_EQ(std::count(clock.sleep_calls.begin(), clock.sleep_calls.end(), 500), 1);
    EXPECT_TRUE(has_log(events, "Max message length: 0x00000206"));
    EXPECT_TRUE(has_log(events, "Flash block size: 0x00001000"));
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, TestWriteSendsValidateNotCommit)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0xA5);
    auto plan = write_plan(FlashOperation::TestWrite, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kDifferingBlock);
    script_flash_init(transport, true);
    script_prog_volt(transport);
    script_block_transfer(transport, *device, image, kDifferingBlock, true);
    script_crc_compare(transport, *device, image, kDifferingBlock);

    RecordingClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::TestWrite);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(std::count(clock.sleep_calls.begin(), clock.sleep_calls.end(), 500), 0);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, WriteFailsOnRejectedEraseResponse)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kDifferingBlock);
    script_flash_init(transport, false);
    script_prog_volt(transport);
    transport.expectWrite(framed(0x25, u32_be(device->fblocks[kDifferingBlock].start)));
    transport.queueRead(framed(0x64));

    RecordingClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(std::count(clock.sleep_calls.begin(), clock.sleep_calls.end(), 500), 1);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, WriteCancelsMidBlockTransfer)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ToggleCancellation cancellation;
    CancelAfterEraseTransport transport(cancellation);
    script_write_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kDifferingBlock);
    script_flash_init(transport, false);
    script_prog_volt(transport);
    transport.expectWrite(framed(0x25, u32_be(device->fblocks[kDifferingBlock].start)));
    transport.queueRead(framed(0x65));

    FakeClock clock;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.flash_buffer_write_attempts_, 0u);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, WriteRejectsCrcResponseMarkedFailed)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    transport.expectWrite(framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00,
                                                    0x00, 0x00, 0x10, 0x00}));
    transport.queueRead(framed(0x42, bytes::Bytes{0x7F}));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(result.error().detail, "ECU marked CRC response failed");
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts_.begin(), transport.read_timeouts_.end(), 50), 0);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, WriteAcceptsFragmentedBlockCrcAndDrainsIt)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        bytes::Bytes payload = u32_be(block.start);
        payload.push_back(0x00);
        payload.push_back(static_cast<bytes::Byte>((block.len >> 16) & 0xFF));
        payload.push_back(static_cast<bytes::Byte>((block.len >> 8) & 0xFF));
        payload.push_back(static_cast<bytes::Byte>(block.len & 0xFF));
        transport.expectWrite(framed(0x02, payload));
        const std::uint32_t crc = fastecu::checksum::crc32(
            bytes::ByteView(image).subspan(block.start, block.len));
        const bytes::Bytes response = crc_response(crc);
        if (block_no == 0)
        {
            transport.queueRead(bytes::ByteView(response).first(10));
            transport.queueRead(bytes::ByteView(response).subspan(10));
        }
        else
        {
            transport.queueRead(response);
        }
        transport.queue_no_frame();
    }

    RecordingClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts_.begin(), transport.read_timeouts_.end(), 50), 1);
    EXPECT_EQ(std::count(clock.sleep_calls.begin(), clock.sleep_calls.end(), 100), 3);
}

TEST(SubaruDensoSh7055_02Executor, WriteAcceptsBlockCrcAfterEmptyInitialRead)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        bytes::Bytes payload = u32_be(block.start);
        payload.push_back(0x00);
        payload.push_back(static_cast<bytes::Byte>((block.len >> 16) & 0xFF));
        payload.push_back(static_cast<bytes::Byte>((block.len >> 8) & 0xFF));
        payload.push_back(static_cast<bytes::Byte>(block.len & 0xFF));
        transport.expectWrite(framed(0x02, payload));
        const std::uint32_t crc = fastecu::checksum::crc32(
            bytes::ByteView(image).subspan(block.start, block.len));
        if (block_no == 0)
        {
            transport.queue_no_frame();
        }
        transport.queueRead(crc_response(crc));
        transport.queue_no_frame();
    }

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts_.begin(), transport.read_timeouts_.end(), 50), 1);
}

TEST(SubaruDensoSh7055_02Executor, WriteRejectsTruncatedBlockCrcAfterBoundedReads)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    transport.expectWrite(framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00,
                                                    0x00, 0x00, 0x10, 0x00}));
    transport.queueRead(bytes::Bytes{0xBE, 0xEF, 0x00, 0x06, 0x42,
                                     0x05, 0x00, 0x00, 0x00, 0x00});
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        transport.queue_no_frame();
    }

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts_.begin(), transport.read_timeouts_.end(), 50), 20);
}

TEST(SubaruDensoSh7055_02Executor, WriteRejectsNegativeBlockCrcResponse)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    transport.expectWrite(framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00,
                                                    0x00, 0x00, 0x10, 0x00}));
    transport.queueRead(framed(0x7F, bytes::Bytes{0x00, 0x00, 0x00, 0x00}));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WritePropagatesBlockCrcDrainError)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    transport.expectWrite(framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00,
                                                    0x00, 0x00, 0x10, 0x00}));
    const std::uint32_t crc = fastecu::checksum::crc32(
        bytes::ByteView(image).first(device->fblocks[0].len));
    transport.queueRead(crc_response(crc));
    transport.queue_error(ErrorKind::Disconnected, "CRC drain failed");

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(result.error().detail, "CRC drain failed");
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteRejectsTruncatedFlashInitResponse)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kBlock);
    transport.expectWrite(framed(0x05));
    transport.queueRead(bytes::Bytes{0xBE, 0xEF, 0x00, 0x05, 0x45,
                                     0xA5, 0x00, 0x00, 0x02});

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteFailsOnRejectedProgVoltResponse)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kBlock);
    script_flash_init(transport, false);
    transport.expectWrite(framed(0x04));
    transport.queueRead(framed(0x7F, bytes::Bytes{0x04, 0xB0}));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteFailsOnRejectedFlashBufferResponse)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_prefix(transport, *device, image, kBlock, false);
    transport.expectWrite(write_chunk_request(*device, image, kBlock, 0));
    transport.queueRead(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x62});

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteFailsOnRejectedCommitResponse)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_prefix(transport, *device, image, kBlock, false);
    for (std::uint32_t offset = 0; offset < 0x1000; offset += 0x200)
    {
        transport.expectWrite(write_chunk_request(*device, image, kBlock, offset));
        transport.queueRead(framed(0x62));
    }
    transport.expectWrite(commit_request(*device, image, kBlock, 0, false));
    transport.queueRead(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x64});

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, TestWriteFailsOnRejectedValidateResponse)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::TestWrite, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_prefix(transport, *device, image, kBlock, true);
    for (std::uint32_t offset = 0; offset < 0x1000; offset += 0x200)
    {
        transport.expectWrite(write_chunk_request(*device, image, kBlock, offset));
        transport.queueRead(framed(0x62));
    }
    transport.expectWrite(commit_request(*device, image, kBlock, 0, true));
    transport.queueRead(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x63});

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteLogsRemainingMismatchAfterVerification)
{
    const flashdev_t *device = find_flash_device("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = write_plan(FlashOperation::Write, image);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_write_connect_and_upload(transport);
    script_crc_compare(transport, *device, image, kBlock);
    script_flash_init(transport, false);
    script_prog_volt(transport);
    script_block_transfer(transport, *device, image, kBlock, false);
    script_crc_compare(transport, *device, image, kBlock);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(has_log(
        events, "Flash verification differs; do not power off, the kernel is still running"));
}

TEST(SubaruDensoSh7055_02Executor, WrxInitLoopExhaustsAfter20Attempts)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_wrx_preamble(transport, true);
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:201-228.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x00});
    }
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:225-228.
    transport.queue_no_frame();

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 22u); // probe + SID BF + 20 WRX requests
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoSh7055_02Executor, CancellationDuringWrxInitLoopStopsBeforeSecondAttempt)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_wrx_preamble(transport, true);
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:201-224.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x00});

    FakeClock clock;
    // This threshold permits the setup I/O and first malformed WRX response,
    // then flips at the loop guard before attempt two.
    FlipAfter cancellation(48);
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 3u); // probe + SID BF + one WRX request
    EXPECT_EQ(transport.close_call_count_, 1);
}

} // namespace
} // namespace fastecu::flash
