#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/testing/faulting_kline_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/faulting_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

using ::testing::_;
using ::testing::Eq;
using ::testing::Optional;

namespace
{
using namespace fastecu;
using namespace fastecu::flash;

bytes::Bytes Frame(bytes::Bytes payload)
{
    return ssm_protocol::AddHeader(payload, 0xf0, 0x10);
}

struct Step
{
    bytes::Bytes request;
    bytes::Bytes reply;
};

std::vector<Step> HandshakeSteps()
{
    return {
        {Frame({0xbf}), bytes::Bytes{0x80, 0xf0, 0x10, 0x09, 0xff, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0}},
        {Frame({0x81}), bytes::Bytes{0, 0, 0, 0, 0xc1}},
        {Frame({0x83, 0x00}), bytes::Bytes{0, 0, 0, 0, 0xc3}},
        {Frame({0x27, 0x01}), bytes::Bytes{0, 0, 0, 0, 0x67, 0x01, 0x12, 0x34, 0x56, 0x78}},
        // Hand-derived seed 0x12345678 -> key 0xF7ABD485.
        {Frame({0x27, 0x02, 0xf7, 0xab, 0xd4, 0x85}), bytes::Bytes{0, 0, 0, 0, 0x67, 0x02}},
        {Frame({0x10, 0x85, 0x02}), bytes::Bytes{0, 0, 0, 0, 0x50}},
    };
}

void ScriptSteps(ScriptedKlineFlashTransport& transport, const std::vector<Step>& steps)
{
    for (const Step& step : steps)
    {
        transport.Exchange(step.request, step.reply);
    }
}

void ScriptHandshake(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.Section("handshake");
    ScriptSteps(transport, HandshakeSteps());
}

bytes::Bytes EncryptedImage(bytes::ByteView image)
{
    static constexpr auto kIndex = std::to_array<std::uint16_t>({0x25b5, 0x3875, 0xca11, 0x2680});
    return ssm_protocol::CalculatePayload(image, kIndex, ssm_protocol::kStockPairTable);
}

TEST(SubaruMitsuM32rKlineExecutor, RejectsFamilyMismatchBeforeIo)
{
    auto plan =
        BuildMitsuColtM32rCanPlan(FlashOperation::kRead, "mitsu_ecu_m32r_can", "M32R_384KB_1block", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    SubaruMitsuM32rKlineExecutor executor;
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                            ::testing::Eq("plan family does not match this executor")));
    EXPECT_THAT(transport.last_config, Eq(std::nullopt));
}

TEST(SubaruMitsuM32rKlineExecutor, TransportSetupReturnsPlansWireParameters)
{
    auto plan = BuildSubaruMitsuM32rKlinePlan(FlashOperation::kRead, "sub_ecu_mitsu_m32r_kline", "M32R_512KB_4blocks",
                                              std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    SubaruMitsuM32rKlineExecutor executor;

    auto setup = executor.TransportSetup(*plan);

    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->baud, 4800);
    EXPECT_FALSE(setup->iso14230);
    EXPECT_EQ(setup->tester_id, 0xf0);
    EXPECT_EQ(setup->target_id, 0x10);
}

TEST(SubaruMitsuM32rKlineExecutor, CancellationBeforeSetupPerformsNoIo)
{
    auto plan = BuildSubaruMitsuM32rKlinePlan(FlashOperation::kRead, "sub_ecu_mitsu_m32r_kline", "M32R_512KB_4blocks",
                                              std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    SubaruMitsuM32rKlineExecutor executor;
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    FakeClock clock;
    ManualCancellationToken cancellation;
    cancellation.Cancel();
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before setup")));
    EXPECT_THAT(transport.last_config, Eq(std::nullopt));
}

TEST(SubaruMitsuM32rKlineExecutor, MapsMissingMalformedAndTransportFailureResponses)
{
    for (const ErrorKind expected : {ErrorKind::kTimeout, ErrorKind::kBadResponse, ErrorKind::kDisconnected})
    {
        SCOPED_TRACE(::testing::PrintToString(expected));
        auto plan = BuildSubaruMitsuM32rKlinePlan(FlashOperation::kRead, "sub_ecu_mitsu_m32r_kline",
                                                  "M32R_512KB_4blocks", std::nullopt);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        transport.Exchange(Frame({0xbf}));
        if (expected == ErrorKind::kTimeout)
        {
            transport.QueueNoFrame();
        }
        else if (expected == ErrorKind::kBadResponse)
        {
            transport.QueueRead(bytes::Bytes{0, 0, 0, 0, 0x7f});
        }
        else
        {
            transport.QueueError(ErrorKind::kDisconnected, "adapter disconnected");
        }
        SubaruMitsuM32rKlineExecutor executor;
        FakeClock clock;
        ManualCancellationToken cancellation;
        RecordingEventSink events;

        ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsErr(expected));
    }
}

TEST(SubaruMitsuM32rKlineExecutor, ReadsAllUserspaceChunksAndSynthesizesBootPrefix)
{
    auto plan = BuildSubaruMitsuM32rKlinePlan(FlashOperation::kRead, "sub_ecu_mitsu_m32r_kline", "M32R_512KB_4blocks",
                                              std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    SubaruMitsuM32rKlineExecutor executor;
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptHandshake(transport);
    for (std::uint32_t address = 0x8000; address < 0x80000; address += 0x80)
    {
        bytes::Bytes response(134, 0xc7);
        response[4] = 0xe0;
        for (std::uint32_t i = 0; i < 0x80; ++i)
        {
            response[5 + i] = static_cast<bytes::Byte>((address + i) * 31U + (address >> 8U));
        }
        transport.Exchange(Frame({0xa0, 0, 0x20, static_cast<bytes::Byte>(address >> 16),
                                  static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address), 0x7f}),
                           response);
    }
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_THAT(result->read_bytes, Optional(_));
    EXPECT_EQ(result->read_bytes->size(), 0x80000U);
    EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(0x8000), test_bytes::BytesFilledWith(0xff));
    bytes::Bytes expected(0x80000, 0xff);
    for (std::uint32_t i = 0x8000; i < expected.size(); ++i)
    {
        expected[i] = static_cast<bytes::Byte>(i * 31U + ((i & ~0x7fU) >> 8U));
    }
    EXPECT_EQ(*result->read_bytes, expected);
    EXPECT_EQ(transport.WritesConsumed(), 6U + 3840U);
    EXPECT_EQ(transport.read_timeouts.size(), 6U + 3840U);
    EXPECT_THAT(events.logs, ::testing::Contains(::testing::Pair(
                                 LogLevel::kWarning,
                                 "The first 0x8000 ROM bytes are synthetic 0xFF; this protocol reads userspace only")));
    EXPECT_EQ(result->rom_id, std::string("123456789A_"));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruMitsuM32rKlineExecutor, WritesEveryEncryptedChunkAndToleratesTransferAcks)
{
    bytes::Bytes image(0x80000);
    for (std::size_t i = 0; i < image.size(); ++i)
    {
        image[i] = static_cast<bytes::Byte>(i);
    }
    const bytes::Bytes encrypted = EncryptedImage(image);
    auto plan =
        BuildSubaruMitsuM32rKlinePlan(FlashOperation::kWrite, "sub_ecu_mitsu_m32r_kline", "M32R_512KB_4blocks", image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptHandshake(transport);
    transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x07, 0x80, 0}), bytes::Bytes{0, 0, 0, 0, 0x74});
    transport.Exchange(Frame({0x31, 0x02, 0x0f, 0xff, 0xff, 0xff}), bytes::Bytes{0, 0, 0, 0, 0x71});
    for (std::uint32_t address = 0x8000; address < 0x80000; address += 0x80)
    {
        bytes::Bytes request{0x36, static_cast<bytes::Byte>(address >> 16), static_cast<bytes::Byte>(address >> 8),
                             static_cast<bytes::Byte>(address)};
        request.insert(request.end(), encrypted.begin() + address, encrypted.begin() + address + 0x80);
        transport.Exchange(Frame(request));
        if (((address - 0x8000) / 0x80) % 2 == 0)
        {
            transport.QueueNoFrame();
        }
        else
        {
            transport.QueueRead(bytes::Bytes{0xde, 0xad});
        }
    }
    transport.Exchange(Frame({0x31, 0x01, 0x02}), bytes::Bytes{0, 0, 0, 0, 0x71, 0x01, 0x02});
    SubaruMitsuM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.baud_calls, std::vector<int>{15625});
}

TEST(SubaruMitsuM32rKlineExecutor, PreservesRequiredResponseTimeoutDetail)
{
    auto plan = BuildSubaruMitsuM32rKlinePlan(FlashOperation::kRead, "sub_ecu_mitsu_m32r_kline", "M32R_512KB_4blocks",
                                              std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40});
    transport.QueueNoFrame();
    SubaruMitsuM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, ::testing::Eq("no response from ECU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(SubaruMitsuM32rKlineExecutor, PreservesPresentEmptyResponseValidation)
{
    auto plan = BuildSubaruMitsuM32rKlinePlan(FlashOperation::kRead, "sub_ecu_mitsu_m32r_kline", "M32R_512KB_4blocks",
                                              std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40});
    transport.QueueRead(bytes::Bytes{});
    SubaruMitsuM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse, ::testing::Eq("No valid response from ECU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(SubaruMitsuM32rKlineExecutor, PreservesExchangeReadFailure)
{
    auto plan = BuildSubaruMitsuM32rKlinePlan(FlashOperation::kRead, "sub_ecu_mitsu_m32r_kline", "M32R_512KB_4blocks",
                                              std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40});
    transport.QueueError(ErrorKind::kDisconnected, "read fault");
    SubaruMitsuM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("read fault")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

// The handshake, then the write's download setup and erase: every exchange
// before the chunk loop.
std::vector<Step> WritePrefixSteps()
{
    std::vector<Step> steps = HandshakeSteps();
    steps.push_back({Frame({0x34, 0, 0, 0, 0x04, 0x07, 0x80, 0}), bytes::Bytes{0, 0, 0, 0, 0x74}});
    steps.push_back({Frame({0x31, 0x02, 0x0f, 0xff, 0xff, 0xff}), bytes::Bytes{0, 0, 0, 0, 0x71}});
    return steps;
}

const bytes::Bytes& WriteImage()
{
    static const bytes::Bytes kImage(0x80000, 0x5a);
    return kImage;
}

const Result<FlashPlan>& WritePlan()
{
    static const Result<FlashPlan> kPlan = BuildSubaruMitsuM32rKlinePlan(
        FlashOperation::kWrite, "sub_ecu_mitsu_m32r_kline", "M32R_512KB_4blocks", WriteImage());
    return kPlan;
}

// Every chunk write of WriteImage(), each acknowledged by silence.
void ScriptChunks(ScriptedKlineFlashTransport& transport)
{
    static const bytes::Bytes kEncrypted = EncryptedImage(WriteImage());
    const auto section = transport.Section("chunks");
    for (std::uint32_t address = 0x8000; address < 0x80000; address += 0x80)
    {
        bytes::Bytes request{0x36, static_cast<bytes::Byte>(address >> 16), static_cast<bytes::Byte>(address >> 8),
                             static_cast<bytes::Byte>(address)};
        request.insert(request.end(), kEncrypted.begin() + address, kEncrypted.begin() + address + 0x80);
        transport.Exchange(Frame(request));
        transport.QueueNoFrame();
    }
}

Result<FlashExecutionResult> RunWrite(ScriptedKlineFlashTransport& transport, IClock& clock,
                                      const ICancellationToken& cancellation)
{
    const Result<FlashPlan>& plan = WritePlan();
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    RecordingEventSink events;
    return SubaruMitsuM32rKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events);
}

TEST(SubaruMitsuM32rKlineExecutor, EveryTransportFaultBeforeTheChunksPropagates)
{
    const std::vector<Step> steps = WritePrefixSteps();
    for (const auto& [op, count] : std::to_array<std::pair<KlineFaultOp, std::size_t>>({
             {KlineFaultOp::kHeaderMode, 1},
             {KlineFaultOp::kSetBaud, 1},
             {KlineFaultOp::kWrite, steps.size()},
             {KlineFaultOp::kRead, steps.size()},
         }))
    {
        SCOPED_TRACE(::testing::PrintToString(static_cast<int>(op)));
        for (std::size_t k = 1; k <= count; ++k)
        {
            SCOPED_TRACE(::testing::PrintToString(std::pair{static_cast<int>(op), k}));
            FaultingKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
            transport.op = op;
            transport.fail_at = k;
            ScriptSteps(transport, steps);
            FakeClock clock;
            ManualCancellationToken cancellation;
            EXPECT_THAT(RunWrite(transport, clock, cancellation),
                        fastecu::testing::IsErrWith(ErrorKind::kDisconnected, Eq("injected")));
            EXPECT_EQ(transport.writes_at_fault, std::optional<std::size_t>(transport.WritesConsumed()));
        }
    }
}

TEST(SubaruMitsuM32rKlineExecutor, AWrongReplyBeforeTheChunksStopsTheRun)
{
    const std::vector<Step> steps = WritePrefixSteps();
    for (std::size_t k = 0; k < steps.size(); ++k)
    {
        SCOPED_TRACE(::testing::PrintToString(k));
        std::vector<Step> script = steps;
        script[k].reply = bytes::Bytes{0, 0, 0, 0, 0x7f, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        script.resize(k + 1);
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        ScriptSteps(transport, script);
        FakeClock clock;
        ManualCancellationToken cancellation;
        EXPECT_THAT(RunWrite(transport, clock, cancellation),
                    fastecu::testing::IsErrWith(ErrorKind::kBadResponse, Eq("Wrong response from ECU")));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruMitsuM32rKlineExecutor, AShortEcuIdOrSeedIsRejected)
{
    for (const auto& [k, reply, detail] : std::to_array<std::tuple<std::size_t, bytes::Bytes, std::string_view>>({
             {0, bytes::Bytes{0, 0, 0, 0, 0xff, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a},
              "ECU ID response does not contain five ID bytes"},
             {3, bytes::Bytes{0, 0, 0, 0, 0x67, 0x01, 0x12, 0x34, 0x56}, "seed response is too short"},
         }))
    {
        SCOPED_TRACE(::testing::PrintToString(k));
        std::vector<Step> script = WritePrefixSteps();
        script[k].reply = reply;
        script.resize(k + 1);
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        ScriptSteps(transport, script);
        FakeClock clock;
        ManualCancellationToken cancellation;
        EXPECT_THAT(RunWrite(transport, clock, cancellation),
                    fastecu::testing::IsErrWith(ErrorKind::kBadResponse, Eq(detail)));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruMitsuM32rKlineExecutor, CancellingBeforeTheChunksWritesNothingMore)
{
    for (std::size_t k = 2;; ++k)
    {
        SCOPED_TRACE(::testing::PrintToString(k));
        ASSERT_LT(k, 128U);
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        ScriptSteps(transport, WritePrefixSteps());
        FakeClock clock;
        FakeCancellationToken cancellation;
        std::size_t checks = 0;
        std::optional<std::size_t> writes_at_cancel;
        cancellation.SetPredicate(
            [&]
            {
                if (++checks == k && !transport.ScriptConsumed())
                {
                    writes_at_cancel = transport.WritesConsumed();
                }
                return writes_at_cancel.has_value();
            });
        const auto result = RunWrite(transport, clock, cancellation);
        if (!writes_at_cancel.has_value())
        {
            break;
        }
        EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(transport.WritesConsumed(), *writes_at_cancel);
    }
}

// The last script read is the erase reply. The exchange's own post-read check
// passes; the next one reports "cancelled after erase" before any chunk.
TEST(SubaruMitsuM32rKlineExecutor, CancellingAfterTheEraseSendsNoChunk)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptSteps(transport, WritePrefixSteps());
    FakeClock clock;
    FakeCancellationToken cancellation;
    int checks_after_erase = 0;
    cancellation.SetPredicate([&] { return transport.ScriptConsumed() && ++checks_after_erase > 1; });
    EXPECT_THAT(RunWrite(transport, clock, cancellation),
                fastecu::testing::IsErrWith(ErrorKind::kCancelled, Eq("cancelled after erase")));
    EXPECT_EQ(transport.WritesConsumed(), WritePrefixSteps().size());
}

TEST(SubaruMitsuM32rKlineExecutor, EverySleepFailurePropagates)
{
    // Sleeps: the 100 ms before the ECU ID, the 1000 ms before the checksum.
    for (const std::size_t k : {1U, 2U})
    {
        SCOPED_TRACE(::testing::PrintToString(k));
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        ScriptSteps(transport, WritePrefixSteps());
        ScriptChunks(transport);
        FaultingClock clock;
        clock.fail_at = k;
        ManualCancellationToken cancellation;
        EXPECT_THAT(RunWrite(transport, clock, cancellation),
                    fastecu::testing::IsErrWith(ErrorKind::kDisconnected, Eq("sleep failed")));
        EXPECT_EQ(transport.WritesConsumed(), k == 1 ? 0U : 8U + 3840U);
    }
}

TEST(SubaruMitsuM32rKlineExecutor, AMissingOrWrongChecksumReplyFailsTheWrite)
{
    for (const bool silent : {true, false})
    {
        SCOPED_TRACE(::testing::PrintToString(silent));
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        ScriptSteps(transport, WritePrefixSteps());
        ScriptChunks(transport);
        transport.ExpectWrite(Frame({0x31, 0x01, 0x02}));
        if (silent)
        {
            transport.QueueNoFrame();
        }
        else
        {
            transport.QueueRead(bytes::Bytes{0, 0, 0, 0, 0x71, 0x01, 0x03});
        }
        FakeClock clock;
        ManualCancellationToken cancellation;
        const auto result = RunWrite(transport, clock, cancellation);
        if (silent)
        {
            EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kTimeout));
        }
        else
        {
            EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, Eq("Wrong response from ECU")));
        }
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruMitsuM32rKlineExecutor, AFailedChunkReadFailsTheRead)
{
    auto plan = BuildSubaruMitsuM32rKlinePlan(FlashOperation::kRead, "sub_ecu_mitsu_m32r_kline", "M32R_512KB_4blocks",
                                              std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptHandshake(transport);
    transport.Exchange(Frame({0xa0, 0, 0x20, 0x00, 0x80, 0x00, 0x7f}));
    transport.QueueNoFrame();
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    EXPECT_THAT(SubaruMitsuM32rKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
}
} // namespace
