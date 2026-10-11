#include "src/backend/ports/testing/result_matchers.h"
// Equivalence tests for SubaruDensoSh7055_02Executor, the portable
// replacement for flash_ecu_subaru_denso_sh7055_02_operation.cpp.
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_executor.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/mock_clock.h"
#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::ElementsAre;
using ::testing::Return;

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::ComposeBeWithChecksum;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;

class ToggleCancellation final : public ICancellationToken
{
  public:
    bool Cancelled() const override
    {
        return cancelled_;
    }

    void Cancel()
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

    bool Cancelled() const override
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
        : ScriptedKlineFlashTransport(fastecu::flash::ScriptedTransportInitialState::kOpen), cancellation_(cancellation)
    {
    }

    Result<std::size_t> Write(bytes::ByteView data) override
    {
        if (data.size() > 4 && data[0] == 0xBE && data[1] == 0xEF)
        {
            erase_response_pending_ = data[4] == 0x25;
            if (data[4] == 0x22)
            {
                ++flash_buffer_write_attempts;
            }
        }
        return ScriptedKlineFlashTransport::Write(data);
    }

    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        Result<OptionalBytes> result = ScriptedKlineFlashTransport::Read(timeout, cancellation);
        if (erase_response_pending_)
        {
            erase_response_pending_ = false;
            cancellation_.Cancel();
        }
        return result;
    }

    std::size_t flash_buffer_write_attempts = 0;

  private:
    ToggleCancellation& cancellation_;
    bool erase_response_pending_ = false;
};

Result<FlashPlan> ReadPlan(bytes::Bytes kernel_bytes = {0x01, 0x02, 0x03, 0x04, 0x05})
{
    return BuildSubaruDensoSh705502Plan(
        FlashOperation::kRead, "sub_ecu_denso_sh7055_02", "SH7055", std::nullopt,
        KernelImage{.id = "k", .load_address = 0xFFFF6004, .bytes = std::move(kernel_bytes)});
}

Result<FlashPlan> WritePlan(FlashOperation operation = FlashOperation::kWrite, bytes::Bytes image = {})
{
    const int index = FindFlashDeviceIndex("SH7055");
    if (index < 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "SH7055 fixture is missing");
    }
    if (image.empty())
    {
        image.resize(kFlashDevices[index].romsize, bytes::Byte{0});
    }
    return BuildSubaruDensoSh705502Plan(
        operation, "sub_ecu_denso_sh7055_02", "SH7055", std::move(image),
        KernelImage{.id = "k", .load_address = 0xFFFF6004, .bytes = {0x01, 0x02, 0x03, 0x04, 0x05}});
}

Result<FlashPlan> MalformedPlan(std::vector<ConfirmationSpec> confirmations, std::uint8_t tester_id = 0xF0)
{
    const int index = FindFlashDeviceIndex("SH7055");
    if (index < 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "SH7055 fixture is missing");
    }
    return ValidateAndBuild(FlashPlanFields{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kSubaruDensoSh705502,
        .transport = TransportKind::kKline,
        .target_id = "sub_ecu_denso_sh7055_02",
        .mcu_name = "SH7055",
        .transfer_region = {kFlashDevices[index].fblocks[0].start, kFlashDevices[index].romsize},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0xFFFF6004, .bytes = {0x01}},
        .family_plan =
            SubaruDensoSh7055_02Plan{
                .tester_id = tester_id,
                .target_id = 0x10,
                .read_ecu_id = true,
            },
        .confirmations = std::move(confirmations),
    });
}

bytes::Bytes Framed(std::uint8_t opcode, bytes::ByteView payload = {})
{
    const std::uint16_t length = static_cast<std::uint16_t>(payload.size() + 1);
    return ComposeBeWithChecksum(bytes::Sum8, std::uint16_t{0xBEEF}, length, bytes::Byte(opcode), payload);
}

// Anchors framed() against hardcoded wire bytes so a bug in composeBeWithChecksum
// (e.g. the wrong checksum span) cannot move both this helper and the
// production frame() it stands in for together and hide behind a passing suite.
//
// framed(0x01): [0xBE, 0xEF, length_hi, length_lo, 0x01], length = 0+1 = 1.
// Checksum (sum8) over [0xBE, 0xEF, 0x00, 0x01, 0x01]:
//   0xBE + 0xEF + 0x00 + 0x01 + 0x01 = 0x1AF -> & 0xFF = 0xAF.
TEST(SubaruDensoSh7055_02Executor, FramedHelperMatchesHardcodedWireBytesNoPayload)
{
    EXPECT_THAT(Framed(0x01), ElementsAre(0xBE, 0xEF, 0x00, 0x01, 0x01, 0xAF));
}

// framed(0x02, {0xAB, 0xCD}): length = 2+1 = 3.
// Checksum (sum8) over [0xBE, 0xEF, 0x00, 0x03, 0x02, 0xAB, 0xCD]:
//   0xBE + 0xEF + 0x00 + 0x03 + 0x02 + 0xAB + 0xCD = 0x32A -> & 0xFF = 0x2A.
TEST(SubaruDensoSh7055_02Executor, FramedHelperMatchesHardcodedWireBytesWithPayload)
{
    EXPECT_THAT(Framed(0x02, bytes::Bytes{0xAB, 0xCD}), ElementsAre(0xBE, 0xEF, 0x00, 0x03, 0x02, 0xAB, 0xCD, 0x2A));
}

bytes::Bytes EcuIdResponse()
{
    // byte 4 is the positive-response marker and the five ECU-ID bytes are
    // sliced from offsets 8..12. The expected hexadecimal ID is 4142434445.
    return {0x80, 0x10, 0xF0, 0x09, 0xFF, 0x00, 0x00, 0x00, 0x41, 0x42, 0x43, 0x44, 0x45, 0x00};
}

bytes::Bytes ExactUploadRequest()
{
    // Five input bytes are padded to eight. The inner checksum is 0x4B at
    // offset 7 and the checksum over the complete message is the final 0xDF.
    return {0x53, 0xFF, 0x60, 0x00, 0x00, 0x0C, 0x65, 0x4B, 0x31, 0x61,
            0x64, 0x67, 0x66, 0x61, 0x60, 0x65, 0x65, 0x65, 0xDF};
}

bytes::Bytes CrcResponse(std::uint32_t crc)
{
    return Framed(0x42, ComposeBe(0x05_b, crc));
}

void ScriptReadPage(ScriptedKlineFlashTransport& transport, std::uint32_t address, bytes::Byte fill,
                    std::uint8_t response_opcode = 0x43)
{
    const auto section = transport.Section("read page");
    // SUB_KERNEL_READ_AREA uses a zero byte plus the 24-bit address and a
    // fixed 0x400-byte page request. The reply carries the 0x43 acknowledgment.
    transport.Exchange(Framed(0x03, ComposeBe(0x00_b, U24(address), std::uint16_t{0x400})),
                       Framed(response_opcode, bytes::Bytes(0x400, fill)));
}

void ScriptFailedProbe(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.Section("failed probe");
    transport.Exchange(Framed(0x01));
    transport.QueueNoFrame();
}

void ScriptWrxPreamble(ScriptedKlineFlashTransport& transport, bool read_ecu_id)
{
    const auto section = transport.Section("wrx preamble");
    transport.QueueNoFrame();
    ScriptFailedProbe(transport);
    if (read_ecu_id)
    {
        transport.Exchange(bytes::Bytes{0x80, 0x10, 0xF0, 0x01, 0xBF, 0x40}, EcuIdResponse());
    }
    transport.QueueNoFrame();
    transport.QueueNoFrame();
}

void ScriptFirstWrxAttemptConnects(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.Section("first wrx attempt connects");
    // check_received_message() returns zero only for an exact
    // three-byte match, so !check_received_message(...) means connected.
    transport.Exchange(bytes::Bytes{0x4D, 0xFF, 0xB4}, bytes::Bytes{0x4D, 0x00, 0xB3});
    transport.QueueNoFrame();
}

void ScriptUpload(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.Section("upload");
    transport.Exchange(ExactUploadRequest());
    transport.QueueNoFrame();
    transport.Exchange(Framed(0x01), Framed(0x41, bytes::Bytes{'K', 'I', 'D'}));
}

void ScriptWriteConnectAndUpload(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.Section("write connect and upload");
    ScriptWrxPreamble(transport, false);
    ScriptFirstWrxAttemptConnects(transport);
    ScriptUpload(transport);
}

void ScriptCrcCompare(ScriptedKlineFlashTransport& transport, const FlashDevice& device, bytes::ByteView image,
                      std::optional<unsigned> differing_block)
{
    const auto section = transport.Section("crc compare");
    // Legacy check_romcrc(): the CRC request uses a 32-bit address, a zero
    // prefix, and a 24-bit block length. The response is accumulated to ten
    // bytes, unwrapped from its BEEF envelope, and drained.
    for (unsigned block_no = 0; block_no < device.numblocks; ++block_no)
    {
        const auto& block = device.fblocks[block_no];
        const bytes::Bytes request_payload = ComposeBe(block.start, 0x00_b, U24(block.len));
        transport.Exchange(Framed(0x02, request_payload));

        std::uint32_t ecu_crc = fastecu::checksum::Crc32(bytes::ByteView(image).subspan(block.start, block.len));
        if (differing_block == block_no)
        {
            ecu_crc ^= 0x00000001U;
        }
        transport.QueueRead(CrcResponse(ecu_crc));
        transport.QueueNoFrame();
    }
}

void ScriptFlashInit(ScriptedKlineFlashTransport& transport, bool test_write)
{
    const auto section = transport.Section("flash init");
    // Legacy init_flash_write(). SH7055's 32-bit values are at response
    // offsets 6..9, so byte 5 is a deliberately non-zero prefix that would
    // expose accidental MC68-style 5..8 parsing.
    transport.Exchange(Framed(0x05), Framed(0x45, bytes::Bytes{0xA5, 0x00, 0x00, 0x02, 0x06}));
    transport.Exchange(Framed(0x06), Framed(0x46, bytes::Bytes{0x5A, 0x00, 0x00, 0x10, 0x00}));
    const std::uint8_t enable_opcode = test_write ? 0x21 : 0x20;
    transport.Exchange(Framed(enable_opcode), Framed(static_cast<std::uint8_t>(enable_opcode | 0x40U)));
}

void ScriptProgVolt(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.Section("prog volt");
    // Legacy reflash_block().
    transport.Exchange(Framed(0x04), Framed(0x44, bytes::Bytes{0x04, 0xB0}));
}

bytes::Bytes WriteChunkRequest(const FlashDevice& device, bytes::ByteView image, unsigned block_no,
                               std::uint32_t offset)
{
    constexpr std::uint32_t kChunkSize = 0x200;
    const auto& block = device.fblocks[block_no];
    // The image bytes are spliced with a raw insert rather than folded into
    // composeBe: production builds this frame as composeBe(address, subspan),
    // so folding would make both sides the same expression and a bug in the
    // splice would cancel out instead of failing the test.
    bytes::Bytes payload = ComposeBe(block.start + offset);
    payload.insert(payload.end(), image.begin() + block.start + offset,
                   image.begin() + block.start + offset + kChunkSize);
    return Framed(0x22, payload);
}

bytes::Bytes CommitRequest(const FlashDevice& device, bytes::ByteView image, unsigned block_no, std::uint32_t offset,
                           bool test_write)
{
    constexpr std::uint32_t kCommitSize = 0x1000;
    const auto& block = device.fblocks[block_no];
    const std::uint32_t address = block.start + offset;
    const std::uint32_t crc = fastecu::checksum::Crc32(bytes::ByteView(image).subspan(address, kCommitSize));
    // 0x10, 0x00 stay two byte literals: production spells this field
    // std::uint16_t(kCommitBlockSize), so the width derivation is not shared
    // and a byte-order bug in appendU16Be would fail this test rather than
    // move both sides together.
    return Framed(test_write ? 0x23 : 0x24, ComposeBe(address, 0x10_b, 0x00_b, crc));
}

void ScriptBlockTransfer(ScriptedKlineFlashTransport& transport, const FlashDevice& device, bytes::ByteView image,
                         unsigned block_no, bool test_write)
{
    const auto section = transport.Section("block transfer");
    constexpr std::uint32_t kChunkSize = 0x200;
    constexpr std::uint32_t kCommitSize = 0x1000;
    const auto& block = device.fblocks[block_no];
    if (!test_write)
    {
        transport.Exchange(Framed(0x25, ComposeBe(block.start)), Framed(0x65));
    }
    for (std::uint32_t offset = 0; offset < block.len; offset += kChunkSize)
    {
        transport.Exchange(WriteChunkRequest(device, image, block_no, offset), Framed(0x62));
        if ((offset + kChunkSize) % kCommitSize == 0)
        {
            const std::uint32_t commit_offset = offset + kChunkSize - kCommitSize;
            const std::uint8_t opcode = test_write ? 0x23 : 0x24;
            transport.Exchange(CommitRequest(device, image, block_no, commit_offset, test_write),
                               Framed(static_cast<std::uint8_t>(opcode | 0x40U)));
        }
    }
    transport.QueueNoFrame();
}

void ScriptWritePrefix(ScriptedKlineFlashTransport& transport, const FlashDevice& device, bytes::ByteView image,
                       unsigned block_no, bool test_write)
{
    const auto section = transport.Section("write prefix");
    ScriptWriteConnectAndUpload(transport);
    ScriptCrcCompare(transport, device, image, block_no);
    ScriptFlashInit(transport, test_write);
    ScriptProgVolt(transport);
    if (!test_write)
    {
        transport.Exchange(Framed(0x25, ComposeBe(device.fblocks[block_no].start)), Framed(0x65));
    }
}

bool HasLog(const RecordingEventSink& events, std::string_view message)
{
    return std::ranges::any_of(events.logs, [message](const auto& entry) { return entry.second == message; });
}

TEST(SubaruDensoSh7055_02Executor, TransportSetupReturnsPlansWireParameters)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    SubaruDensoSh7055_02Executor executor;

    const auto setup = executor.TransportSetup(*plan);

    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->baud, 62500);
    EXPECT_FALSE(setup->iso14230);
    EXPECT_EQ(setup->tester_id, 0xF0);
    EXPECT_EQ(setup->target_id, 0x10);
}

TEST(SubaruDensoSh7055_02Executor, BoundAttemptPreservesBothConfigureToOpenCancellationCheckpoints)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto transport = std::make_unique<ScriptedKlineFlashTransport>();
    auto *observed_transport = transport.get();
    auto attempt =
        BindFlashAttempt(std::move(*plan), std::make_unique<SubaruDensoSh7055_02Executor>(), std::move(transport));
    FakeClock clock;
    FlipAfter cancellation(2);
    RecordingEventSink events;

    ASSERT_THAT(attempt->Run(clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(observed_transport->last_config.has_value());
    EXPECT_EQ(observed_transport->close_call_count, 0);
    EXPECT_TRUE(observed_transport->control_line_trace.empty());
}

TEST(SubaruDensoSh7055_02Executor, KernelAlreadyAliveSkipsWrxInitEcuIdAndUpload)
{
    auto plan = WritePlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.QueueNoFrame();
    transport.Exchange(Framed(0x01), Framed(0x41, bytes::Bytes{'K'}));
    ScriptCrcCompare(transport, *device, plan->ImageOrEmpty(), std::nullopt);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, KernelAliveReadReturnsNoRomId)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.QueueNoFrame();
    transport.Exchange(Framed(0x01), Framed(0x41, bytes::Bytes{'K'}));
    for (std::uint32_t offset = 0; offset < device->romsize; offset += 0x400)
    {
        ScriptReadPage(transport, device->fblocks[0].start + offset, 0x5a);
    }

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), device->romsize);
    EXPECT_FALSE(result->rom_id.has_value());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_TRUE(transport.baud_calls.empty());
}

TEST(SubaruDensoSh7055_02Executor, RejectsMissingConfirmationAndMalformedFamilyBeforeTransportIo)
{
    for (auto plan : {
             MalformedPlan({}),
             MalformedPlan({ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition}}, 0xF1),
         })
    {
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;
        SubaruDensoSh7055_02Executor executor;

        ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
        EXPECT_FALSE(transport.last_config.has_value());
        EXPECT_EQ(transport.WritesConsumed(), 0U);
        EXPECT_TRUE(transport.read_timeouts.empty());
        EXPECT_TRUE(transport.control_line_trace.empty());
    }
}

TEST(SubaruDensoSh7055_02Executor, ReadSurfacesEcuIdInResult)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.post_kernel_upload_delay_required = true;
    ScriptWrxPreamble(transport, true);
    ScriptFirstWrxAttemptConnects(transport);
    ScriptUpload(transport);
    const int device_index = FindFlashDeviceIndex("SH7055");
    ASSERT_GE(device_index, 0);
    const auto& device = kFlashDevices[device_index];
    for (std::uint32_t offset = 0; offset < device.romsize; offset += 0x400)
    {
        ScriptReadPage(transport, device.fblocks[0].start + offset, 0x5A);
    }

    RecordingClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->rom_id, std::optional<std::string>{"4142434445"});
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_TRUE(HasLog(events, "ECU ID: 4142434445"));
    EXPECT_EQ(transport.baud_calls, (std::vector<int>{4800, 9600, 62500}));
    EXPECT_EQ(transport.control_line_trace, (std::vector<ScriptedKlineFlashTransport::ControlLineAction>{
                                                ScriptedKlineFlashTransport::ControlLineAction::kDisableLecLines,
                                                ScriptedKlineFlashTransport::ControlLineAction::kDisableLecLines,
                                                ScriptedKlineFlashTransport::ControlLineAction::kPulseLec2,
                                            }));
    EXPECT_EQ(transport.lec_2_pulse_timeouts, (std::vector<std::chrono::milliseconds>{200ms}));
    std::vector<std::chrono::milliseconds> expected_sleeps{200ms, 1000ms, 1000ms, 1000ms, 250ms,
                                                           190ms, 100ms,  5000ms, 100ms,  200ms};
    std::vector<std::chrono::milliseconds> expected_timeouts{10ms, 2000ms, 2000ms, 10ms,  10ms,
                                                             10ms, 10ms,   200ms,  2000ms};
    for (std::uint32_t offset = 0; offset < device.romsize; offset += 0x400)
    {
        expected_sleeps.insert(expected_sleeps.end(), {10ms, 1ms});
        expected_timeouts.push_back(3000ms);
    }
    EXPECT_EQ(clock.sleep_calls, expected_sleeps);
    EXPECT_EQ(transport.read_timeouts, expected_timeouts);
}

TEST(SubaruDensoSh7055_02Executor, OpenPort2UploadDelayCancellationStopsBeforeResponseRead)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    transport.post_kernel_upload_delay_required = true;
    ScriptWrxPreamble(transport, true);
    ScriptFirstWrxAttemptConnects(transport);
    transport.Exchange(ExactUploadRequest());

    MockClock clock;
    EXPECT_CALL(clock, Sleep(5000ms, _))
        .WillOnce(Return(Fail(ErrorKind::kCancelled, "cancelled during OpenPort2 upload delay")));
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 200ms), 0);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, ReadReturnsAssembledPageBytes)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWrxPreamble(transport, true);
    ScriptFirstWrxAttemptConnects(transport);
    ScriptUpload(transport);
    const int device_index = FindFlashDeviceIndex("SH7055");
    ASSERT_GE(device_index, 0);
    const auto& device = kFlashDevices[device_index];

    bytes::Bytes expected;
    for (std::uint32_t offset = 0; offset < device.romsize; offset += 0x400)
    {
        const bytes::Byte fill = static_cast<bytes::Byte>(offset / 0x400);
        ScriptReadPage(transport, device.fblocks[0].start + offset, fill);
        expected.insert(expected.end(), 0x400, fill);
    }

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, expected);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, ReadRejectsMalformedPageResponse)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWrxPreamble(transport, true);
    ScriptFirstWrxAttemptConnects(transport);
    ScriptUpload(transport);
    // the 0x43 acknowledgment is required before stripping the frame envelope.
    transport.Exchange(Framed(0x03, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x04, 0x00}),
                       Framed(0x44, bytes::Bytes(0x400, 0xA5)));

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, ReadRejectsTruncatedPageResponse)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWrxPreamble(transport, true);
    ScriptFirstWrxAttemptConnects(transport);
    ScriptUpload(transport);
    // The reply has the valid 0x43 envelope, but its payload is shorter than
    // the requested 0x400-byte legacy page, so it must not yield a silently
    // undersized ROM.
    transport.Exchange(Framed(0x03, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x04, 0x00}),
                       Framed(0x43, bytes::Bytes{0xA5}));

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, ReadCancelsBetweenPages)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWrxPreamble(transport, true);
    ScriptFirstWrxAttemptConnects(transport);
    ScriptUpload(transport);
    ScriptReadPage(transport, 0x00000000, 0xA5);

    ToggleCancellation cancellation;
    MockClock clock;
    // Cancel only after the first page's 1 ms pacing sleep has completed.
    EXPECT_CALL(clock, Sleep(1ms, _))
        .WillOnce(DoAll(clock.SleepOnFake(), [&] { cancellation.Cancel(); }, Return(Status{})));
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 6U); // probe + SID BF + WRX + upload + kernel ID + first read
}

TEST(SubaruDensoSh7055_02Executor, NoFrameWrxReplyRetriesUntilExactResponse)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWrxPreamble(transport, true);
    // an empty response is not the exact three-byte success.
    transport.Exchange(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.QueueNoFrame();
    ScriptFirstWrxAttemptConnects(transport);
    ScriptUpload(transport);
    const int device_index = FindFlashDeviceIndex("SH7055");
    ASSERT_GE(device_index, 0);
    const auto& device = kFlashDevices[device_index];
    for (std::uint32_t offset = 0; offset < device.romsize; offset += 0x400)
    {
        ScriptReadPage(transport, device.fblocks[0].start + offset, 0x5A);
    }

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 518U); // probe + SID BF + two WRX + upload + kernel ID + 512 reads
}

TEST(SubaruDensoSh7055_02Executor, WritePathSkipsEcuIdRead)
{
    auto plan = WritePlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, plan->ImageOrEmpty(), std::nullopt);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_FALSE(HasLog(events, "ECU ID: 4142434445"));
    EXPECT_EQ(transport.baud_calls, (std::vector<int>{9600, 62500}));
}

TEST(SubaruDensoSh7055_02Executor, WriteSkipsWhenNoBlockDiffers)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, std::nullopt);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 4U + device->numblocks);
    EXPECT_EQ(transport.programming_voltage_line_write_index, 4U + device->numblocks);
}

TEST(SubaruDensoSh7055_02Executor, WriteReflashesOnlyDifferingBlocks)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0x00);
    const auto& block = device->fblocks[kDifferingBlock];
    for (std::size_t offset = 0; offset < block.len; ++offset)
    {
        image[block.start + offset] = static_cast<bytes::Byte>(offset * 17U + 3U);
    }
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kDifferingBlock);
    ScriptFlashInit(transport, false);
    ScriptProgVolt(transport);
    ScriptBlockTransfer(transport, *device, image, kDifferingBlock, false);
    ScriptCrcCompare(transport, *device, image, std::nullopt);

    MockClock clock;

    EXPECT_CALL(clock, Sleep(50ms, _)).Times(static_cast<int>(block.len / 0x200));

    EXPECT_CALL(clock, Sleep(500ms, _)).Times(1);
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.control_line_trace.back(),
              ScriptedKlineFlashTransport::ControlLineAction::kEnableProgrammingVoltageLine);
    EXPECT_TRUE(HasLog(events, "Max message length: 0x00000206"));
    EXPECT_TRUE(HasLog(events, "Flash block size: 0x00001000"));
}

TEST(SubaruDensoSh7055_02Executor, TestWriteSendsValidateNotCommit)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0xA5);
    auto plan = WritePlan(FlashOperation::kTestWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kDifferingBlock);
    ScriptFlashInit(transport, true);
    ScriptProgVolt(transport);
    ScriptBlockTransfer(transport, *device, image, kDifferingBlock, true);
    ScriptCrcCompare(transport, *device, image, kDifferingBlock);

    MockClock clock;

    EXPECT_CALL(clock, Sleep(500ms, _)).Times(0);
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kTestWrite);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteFailsOnRejectedEraseResponse)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kDifferingBlock);
    ScriptFlashInit(transport, false);
    ScriptProgVolt(transport);
    transport.Exchange(Framed(0x25, ComposeBe(device->fblocks[kDifferingBlock].start)), Framed(0x64));

    MockClock clock;

    EXPECT_CALL(clock, Sleep(500ms, _)).Times(1);
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteCancelsMidBlockTransfer)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ToggleCancellation cancellation;
    CancelAfterEraseTransport transport(cancellation);
    ScriptWriteConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kDifferingBlock);
    ScriptFlashInit(transport, false);
    ScriptProgVolt(transport);
    transport.Exchange(Framed(0x25, ComposeBe(device->fblocks[kDifferingBlock].start)), Framed(0x65));

    FakeClock clock;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.flash_buffer_write_attempts, 0U);
}

TEST(SubaruDensoSh7055_02Executor, WriteRejectsCrcResponseMarkedFailed)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    transport.Exchange(Framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00}),
                       Framed(0x42, bytes::Bytes{0x7F}));

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse, "ECU marked CRC response failed"));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 50ms), 0);
}

TEST(SubaruDensoSh7055_02Executor, WriteAcceptsFragmentedBlockCrcAndDrainsIt)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        const bytes::Bytes payload = ComposeBe(block.start, 0x00_b, U24(block.len));
        transport.Exchange(Framed(0x02, payload));
        const std::uint32_t crc = fastecu::checksum::Crc32(bytes::ByteView(image).subspan(block.start, block.len));
        const bytes::Bytes response = CrcResponse(crc);
        if (block_no == 0)
        {
            transport.QueueRead(bytes::ByteView(response).first(10));
            transport.QueueRead(bytes::ByteView(response).subspan(10));
        }
        else
        {
            transport.QueueRead(response);
        }
        transport.QueueNoFrame();
    }

    MockClock clock;

    EXPECT_CALL(clock, Sleep(100ms, _)).Times(3);
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 50ms), 1);
}

TEST(SubaruDensoSh7055_02Executor, WriteAcceptsBlockCrcAfterEmptyInitialRead)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        const bytes::Bytes payload = ComposeBe(block.start, 0x00_b, U24(block.len));
        transport.Exchange(Framed(0x02, payload));
        const std::uint32_t crc = fastecu::checksum::Crc32(bytes::ByteView(image).subspan(block.start, block.len));
        if (block_no == 0)
        {
            transport.QueueNoFrame();
        }
        transport.QueueRead(CrcResponse(crc));
        transport.QueueNoFrame();
    }

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 50ms), 1);
}

TEST(SubaruDensoSh7055_02Executor, WriteRejectsTruncatedBlockCrcAfterBoundedReads)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    transport.Exchange(Framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00}),
                       bytes::Bytes{0xBE, 0xEF, 0x00, 0x06, 0x42, 0x05, 0x00, 0x00, 0x00, 0x00});
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        transport.QueueNoFrame();
    }

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 50ms), 20);
}

TEST(SubaruDensoSh7055_02Executor, WriteRejectsNegativeBlockCrcResponse)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    transport.Exchange(Framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00}),
                       Framed(0x7F, bytes::Bytes{0x00, 0x00, 0x00, 0x00}));

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WritePropagatesBlockCrcDrainError)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    const std::uint32_t crc = fastecu::checksum::Crc32(bytes::ByteView(image).first(device->fblocks[0].len));
    transport.Exchange(Framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00}), CrcResponse(crc));
    transport.QueueError(ErrorKind::kDisconnected, "CRC drain failed");

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErrWith(ErrorKind::kDisconnected, "CRC drain failed"));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteRejectsTruncatedFlashInitResponse)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kBlock);
    transport.Exchange(Framed(0x05), bytes::Bytes{0xBE, 0xEF, 0x00, 0x05, 0x45, 0xA5, 0x00, 0x00, 0x02});

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteFailsOnRejectedProgVoltResponse)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kBlock);
    ScriptFlashInit(transport, false);
    transport.Exchange(Framed(0x04), Framed(0x7F, bytes::Bytes{0x04, 0xB0}));

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteFailsOnRejectedFlashBufferResponse)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWritePrefix(transport, *device, image, kBlock, false);
    transport.Exchange(WriteChunkRequest(*device, image, kBlock, 0), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x62});

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteFailsOnRejectedCommitResponse)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWritePrefix(transport, *device, image, kBlock, false);
    for (std::uint32_t offset = 0; offset < 0x1000; offset += 0x200)
    {
        transport.Exchange(WriteChunkRequest(*device, image, kBlock, offset), Framed(0x62));
    }
    transport.Exchange(CommitRequest(*device, image, kBlock, 0, false), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x64});

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, TestWriteFailsOnRejectedValidateResponse)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kTestWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWritePrefix(transport, *device, image, kBlock, true);
    for (std::uint32_t offset = 0; offset < 0x1000; offset += 0x200)
    {
        transport.Exchange(WriteChunkRequest(*device, image, kBlock, offset), Framed(0x62));
    }
    transport.Exchange(CommitRequest(*device, image, kBlock, 0, true), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x63});

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoSh7055_02Executor, WriteLogsRemainingMismatchAfterVerification)
{
    const FlashDevice *device = FindFlashDevice("SH7055");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = WritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWriteConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kBlock);
    ScriptFlashInit(transport, false);
    ScriptProgVolt(transport);
    ScriptBlockTransfer(transport, *device, image, kBlock, false);
    ScriptCrcCompare(transport, *device, image, kBlock);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_TRUE(HasLog(events, "Flash verification differs; do not power off, the kernel is still running"));
}

TEST(SubaruDensoSh7055_02Executor, WrxInitLoopExhaustsAfter20Attempts)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWrxPreamble(transport, true);
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        transport.Exchange(bytes::Bytes{0x4D, 0xFF, 0xB4}, bytes::Bytes{0x00, 0x00, 0x00});
    }
    transport.QueueNoFrame();

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 22U); // probe + SID BF + 20 WRX requests
}

TEST(SubaruDensoSh7055_02Executor, CancellationDuringWrxInitLoopStopsBeforeSecondAttempt)
{
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{fastecu::flash::ScriptedTransportInitialState::kOpen};
    ScriptWrxPreamble(transport, true);
    transport.Exchange(bytes::Bytes{0x4D, 0xFF, 0xB4}, bytes::Bytes{0x00, 0x00, 0x00});

    FakeClock clock;
    // This threshold permits the first malformed WRX response,
    // then flips at the loop guard before attempt two.
    FlipAfter cancellation(43);
    RecordingEventSink events;
    SubaruDensoSh7055_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 3U); // probe + SID BF + one WRX request
}

} // namespace
} // namespace fastecu::flash
