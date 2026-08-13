#include "src/backend/flash/ecu/subaru_denso_sh7055_02_executor.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "src/algorithms/checksum/checksum_primitives.h"
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

Result<FlashPlan> read_plan(bytes::Bytes kernel_bytes = {0x01, 0x02, 0x03, 0x04, 0x05})
{
    return build_subaru_denso_sh7055_02_plan(
        FlashOperation::Read, "sub_ecu_denso_sh7055_02", "SH7055", std::nullopt,
        KernelImage{.id = "k", .load_address = 0xFFFF6004, .bytes = std::move(kernel_bytes)});
}

Result<FlashPlan> write_plan()
{
    const int index = find_flash_device_index("SH7055");
    if (index < 0)
    {
        return fail(ErrorKind::InvalidConfig, "SH7055 fixture is missing");
    }
    return build_subaru_denso_sh7055_02_plan(
        FlashOperation::Write, "sub_ecu_denso_sh7055_02", "SH7055",
        bytes::Bytes(flashdevices[index].romsize, bytes::Byte{0}),
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
    out.push_back(fastecu::checksum::checksum8(out, false));
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

bool has_log(const RecordingEventSink& events, std::string_view message)
{
    return std::ranges::any_of(events.logs, [message](const auto& entry)
                               { return entry.second == message; });
}

TEST(SubaruDensoSh7055_02Executor, KernelAlreadyAliveSkipsWrxInitEcuIdAndUpload)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:69-70.
    transport.queue_no_frame();
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp:108-126 and 1169-1198.
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K'}));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
    ASSERT_TRUE(transport.last_config_.has_value());
    EXPECT_EQ(transport.last_config_->baud, 62500);
    EXPECT_EQ(transport.last_config_->tester_id, 0xF0);
    EXPECT_EQ(transport.last_config_->target_id, 0x10);
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

TEST(SubaruDensoSh7055_02Executor, ReadPathExtractsEcuIdAndUploadsExactKernelEnvelope)
{
    auto plan = read_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_wrx_preamble(transport, true);
    script_first_wrx_attempt_connects(transport);
    script_upload(transport);

    RecordingClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
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
    EXPECT_EQ(clock.sleep_calls,
              (std::vector<int>{200, 1000, 1000, 1000, 250, 190, 100, 100, 200}));
    EXPECT_EQ(transport.read_timeouts_,
              (std::vector<int>{10, 2000, 2000, 10, 10, 10, 10, 200, 2000}));
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

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 6u); // probe + SID BF + two WRX + upload + kernel ID
}

TEST(SubaruDensoSh7055_02Executor, WritePathSkipsEcuIdRead)
{
    auto plan = write_plan();
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ScriptedKlineFlashTransport transport;
    script_wrx_preamble(transport, false);
    script_first_wrx_attempt_connects(transport);
    script_upload(transport);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_FALSE(has_log(events, "ECU ID: 4142434445"));
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{9600, 62500}));
    EXPECT_EQ(transport.close_call_count_, 1);
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
