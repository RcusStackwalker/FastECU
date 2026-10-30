#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"
#include "src/backend/ports/manual_cancellation_token.h"
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

class TripOnReadTransport final : public ScriptedKlineFlashTransport
{
  public:
    explicit TripOnReadTransport(ManualCancellationToken& source)
        : ScriptedKlineFlashTransport(ScriptedTransportInitialState::kOpen), source_(source)
    {
    }
    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        auto result = ScriptedKlineFlashTransport::Read(timeout, cancellation);
        if (++reads_ == 3)
        {
            source_.Cancel();
        }
        return result;
    }

  private:
    ManualCancellationToken& source_;
    int reads_ = 0;
};

bytes::Bytes Frame(bytes::Bytes payload)
{
    return ssm_protocol::AddHeader(payload, 0xf0, 0x10);
}

bytes::Bytes IdResponse()
{
    return {0x80, 0xf0, 0x10, 0x09, 0xff, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0};
}

void ScriptReadChunks(ScriptedKlineFlashTransport& transport, std::uint32_t end = 0x80000)
{
    const auto section = transport.Section("read chunks");
    for (std::uint32_t logical = 0; logical < end; logical += 0x80)
    {
        const std::uint32_t address = logical + 0x100000;
        bytes::Bytes response(134, 0xc7);
        response[4] = 0xe0;
        for (std::uint32_t i = 0; i < 0x80; ++i)
        {
            response[5 + i] = static_cast<bytes::Byte>((logical + i) * 31U + (logical >> 8U));
        }
        transport.Exchange(Frame({0xa0, 0, 0, static_cast<bytes::Byte>(address >> 16U),
                                  static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address), 0x7f}),
                           response);
    }
}

void ScriptAuthenticatedTail(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.Section("authenticated tail");
    transport.Exchange(Frame({0x83, 0x00}), bytes::Bytes{0, 0, 0, 0, 0xc3});
    transport.Exchange(Frame({0x27, 0x01}), bytes::Bytes{0, 0, 0, 0, 0x67, 0x01, 0x12, 0x34, 0x56, 0x78});
    transport.Exchange(Frame({0x27, 0x02, 0x6c, 0xf8, 0x3a, 0x6a}), bytes::Bytes{0, 0, 0, 0, 0x67});
    transport.Exchange(Frame({0x10, 0x85, 0x02}), bytes::Bytes{0, 0, 0, 0, 0x50});
}

void ScriptNormalAuthenticatedFallback(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.Section("normal authenticated fallback");
    transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}));
    transport.QueueNoFrame();
    transport.Exchange(Frame({0xbf}), IdResponse());
    transport.Exchange(Frame({0x81}), bytes::Bytes{0, 0, 0, 0, 0xc1});
    transport.Exchange(Frame({0x83, 0x00}), bytes::Bytes{0, 0, 0, 0, 0xc3});
    transport.Exchange(Frame({0x27, 0x01}), bytes::Bytes{0, 0, 0, 0, 0x67, 0x01, 0x12, 0x34, 0x56, 0x78});
    transport.Exchange(Frame({0x27, 0x02, 0x6c, 0xf8, 0x3a, 0x6a}), bytes::Bytes{0, 0, 0, 0, 0x67, 0x03});
}

bytes::Bytes EncryptedImage(bytes::ByteView image)
{
    static constexpr auto kIndex = std::to_array<std::uint16_t>({0x78f1, 0x2962, 0x9312, 0x7c03});
    return ssm_protocol::CalculatePayload(image, kIndex, ssm_protocol::kStockPairTable);
}

// EncryptedImage for the last image seen: encrypting 512 KiB per script would
// dominate the sweeps, which script the same image hundreds of times.
const bytes::Bytes& CachedEncryptedImage(bytes::ByteView image)
{
    static bytes::Bytes input;
    static bytes::Bytes output;
    if (!std::ranges::equal(input, image))
    {
        input.assign(image.begin(), image.end());
        output = EncryptedImage(image);
    }
    return output;
}

// Scripts the chunks below `end`; the final checksum follows only when
// `checksum` is set and every chunk is scripted.
void ScriptWriteBody(ScriptedKlineFlashTransport& transport, bytes::ByteView image, bool checksum = true,
                     std::uint32_t end = 0x80000)
{
    const auto section = transport.Section("write body");
    transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}), bytes::Bytes{0, 0, 0, 0, 0x74});
    transport.Exchange(Frame({0x31, 0x02, 0x0f, 0xff, 0xff, 0xff}), bytes::Bytes{0, 0, 0, 0, 0x71, 0x02});
    const bytes::Bytes& encrypted = CachedEncryptedImage(image);
    for (std::uint32_t address = 0; address < end; address += 0x80)
    {
        bytes::Bytes request{0x36, static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                             static_cast<bytes::Byte>(address)};
        request.append_range(bytes::ByteView(encrypted).subspan(address, 0x80));
        transport.Exchange(Frame(request));
        if ((address / 0x80) % 2 == 0)
        {
            transport.QueueNoFrame();
        }
        else
        {
            transport.QueueRead(bytes::Bytes{0xde, 0xad});
        }
    }
    if (checksum && end == 0x80000)
    {
        transport.Exchange(Frame({0x31, 0x01, 0x02}), bytes::Bytes{0x01});
    }
}

TEST(SubaruHitachiM32rKlineExecutor, TransportSetupReturnsPlansWireParameters)
{
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kRead, "sub_ecu_hitachi_m32r_kline",
                                                "M32R_512KB_1block", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    SubaruHitachiM32rKlineExecutor executor;

    auto setup = executor.TransportSetup(*plan);

    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->baud, 4800);
    EXPECT_FALSE(setup->iso14230);
    EXPECT_EQ(setup->tester_id, 0xf0);
    EXPECT_EQ(setup->target_id, 0x10);
}

TEST(SubaruHitachiM32rKlineExecutor, ReadsAt38400ProbeAndReturnsLogicalFullRom)
{
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kRead, "sub_ecu_hitachi_m32r_kline_recovery",
                                                "M32R_512KB_1block", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.Exchange(Frame({0xbf}), IdResponse());
    ScriptReadChunks(transport);
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_THAT(result->read_bytes, Optional(_));
    EXPECT_EQ(result->read_bytes->size(), 0x80000U);
    bytes::Bytes expected(0x80000);
    for (std::uint32_t i = 0; i < expected.size(); ++i)
    {
        expected[i] = static_cast<bytes::Byte>(i * 31U + ((i & ~0x7fU) >> 8U));
    }
    EXPECT_EQ(*result->read_bytes, expected);
    EXPECT_EQ(transport.WritesConsumed(), 1U + 4096U);
    EXPECT_EQ(transport.read_timeouts.size(), 1U + 4096U);
    EXPECT_EQ(result->rom_id, std::string("123456789A_"));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.baud_calls, std::vector<int>{38400});
}

TEST(SubaruHitachiM32rKlineExecutor, RecoveryWakeIsBoundedToOneThousandAttempts)
{
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kWrite, "sub_ecu_hitachi_m32r_kline_recovery",
                                                "M32R_512KB_1block", bytes::Bytes(0x80000));
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    for (int i = 0; i < 1000; ++i)
    {
        transport.Exchange(Frame({0x81}));
        transport.QueueNoFrame();
    }
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 1000U);
}

TEST(SubaruHitachiM32rKlineExecutor, ReadFallsBackThrough4800Initialization)
{
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kRead, "sub_ecu_hitachi_m32r_kline",
                                                "M32R_512KB_1block", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.Exchange(Frame({0xbf}));
    transport.QueueNoFrame();
    transport.Exchange(Frame({0xbf}), IdResponse());
    transport.Exchange(Frame({0xb8, 0, 0, 0, 0x75}), bytes::Bytes{0, 0, 0, 0, 0xf8});
    transport.Exchange(Frame({0xbf}), IdResponse());
    ScriptReadChunks(transport);
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_EQ(transport.baud_calls, (std::vector<int>{38400, 4800, 38400}));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruHitachiM32rKlineExecutor, NormalWriteUsesActiveObkAndToleratesLegacyAcks)
{
    bytes::Bytes image(0x80000);
    for (std::size_t i = 0; i < image.size(); ++i)
    {
        image[i] = static_cast<bytes::Byte>(i);
    }
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kWrite, "sub_ecu_hitachi_m32r_kline",
                                                "M32R_512KB_1block", image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}), bytes::Bytes{0, 0, 0, 0, 0x74, 0x84});
    ScriptWriteBody(transport, image);
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.baud_calls, (std::vector<int>{15625, 15625}));
}

TEST(SubaruHitachiM32rKlineExecutor, NormalFallbackRequiresSecuritySubfunctionTwo)
{
    const bytes::Bytes image(0x80000);
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kWrite, "sub_ecu_hitachi_m32r_kline",
                                                "M32R_512KB_1block", image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    ScriptNormalAuthenticatedFallback(transport);
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruHitachiM32rKlineExecutor, EraseAcknowledgementAccumulatesBoundedFragments)
{
    const bytes::Bytes image(0x80000, 0xa5);
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kWrite, "sub_ecu_hitachi_m32r_kline",
                                                "M32R_512KB_1block", image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}), bytes::Bytes{0, 0, 0, 0, 0x74, 0x84});
    transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}), bytes::Bytes{0, 0, 0, 0, 0x74});
    transport.Exchange(Frame({0x31, 0x02, 0x0f, 0xff, 0xff, 0xff}), bytes::Bytes{0, 0, 0});
    transport.QueueRead(bytes::Bytes{0, 0x71, 0x02});
    const bytes::Bytes encrypted = EncryptedImage(image);
    for (std::uint32_t address = 0; address < 0x80000; address += 0x80)
    {
        bytes::Bytes request{0x36, static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                             static_cast<bytes::Byte>(address)};
        request.append_range(bytes::ByteView(encrypted).subspan(address, 0x80));
        transport.Exchange(Frame(request));
        transport.QueueNoFrame();
    }
    transport.Exchange(Frame({0x31, 0x01, 0x02}), bytes::Bytes{1});
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruHitachiM32rKlineExecutor, RecoveryWriteWakesAndUsesAuthenticatedSession)
{
    const bytes::Bytes image(0x80000, 0xa5);
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kWrite, "sub_ecu_hitachi_m32r_kline_recovery",
                                                "M32R_512KB_1block", image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.Exchange(Frame({0x81}));
    transport.QueueNoFrame();
    transport.Exchange(Frame({0x81}), bytes::Bytes{0, 0, 0, 0, 0xc1});
    ScriptAuthenticatedTail(transport);
    ScriptWriteBody(transport, image);
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.baud_calls, (std::vector<int>{4800, 15625}));
}

TEST(SubaruHitachiM32rKlineExecutor, CancellationBeforeSetupPerformsNoIo)
{
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kRead, "sub_ecu_hitachi_m32r_kline",
                                                "M32R_512KB_1block", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    cancellation.Cancel();
    RecordingEventSink events;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before setup")));
    EXPECT_THAT(transport.last_config, Eq(std::nullopt));
}

TEST(SubaruHitachiM32rKlineExecutor, CancellationAfterEraseIsNotReportedAsSuccess)
{
    const bytes::Bytes image(0x80000);
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kWrite, "sub_ecu_hitachi_m32r_kline",
                                                "M32R_512KB_1block", image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ManualCancellationToken cancellation;
    TripOnReadTransport transport(cancellation);
    transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}), bytes::Bytes{0, 0, 0, 0, 0x74, 0x84});
    transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}), bytes::Bytes{0, 0, 0, 0, 0x74});
    transport.Exchange(Frame({0x31, 0x02, 0x0f, 0xff, 0xff, 0xff}), bytes::Bytes{0, 0, 0, 0, 0x71, 0x02});
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    RecordingEventSink events;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
}

TEST(SubaruHitachiM32rKlineExecutor, PreservesPrefixFailureDetails)
{
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kRead, "sub_ecu_hitachi_m32r_kline",
                                                "M32R_512KB_1block", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    for (const auto& response : {bytes::Bytes{0, 0, 0, 0}, bytes::Bytes{0, 0, 0, 0, 0xfe}})
    {
        SCOPED_TRACE(response.size());
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40});
        transport.QueueNoFrame();
        transport.Exchange(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40}, response);
        SubaruHitachiM32rKlineExecutor executor;
        FakeClock clock;
        ManualCancellationToken cancellation;
        RecordingEventSink events;

        const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
        EXPECT_THAT(result,
                    fastecu::testing::IsErrWith(
                        ErrorKind::kBadResponse,
                        ::testing::Eq(response.size() == 4 ? "response is too short" : "wrong response from ECU")));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruHitachiM32rKlineExecutor, PreservesRequiredResponseTimeoutDetail)
{
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kRead, "sub_ecu_hitachi_m32r_kline",
                                                "M32R_512KB_1block", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40});
    transport.QueueNoFrame();
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40});
    transport.QueueNoFrame();
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, ::testing::Eq("no response from ECU")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 2U);
}

TEST(SubaruHitachiM32rKlineExecutor, PreservesPresentEmptyResponseValidation)
{
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kRead, "sub_ecu_hitachi_m32r_kline",
                                                "M32R_512KB_1block", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40});
    transport.QueueNoFrame();
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40});
    transport.QueueRead(bytes::Bytes{});
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, ::testing::Eq("response is too short")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 2U);
}

TEST(SubaruHitachiM32rKlineExecutor, PreservesExchangeReadFailure)
{
    auto plan = BuildSubaruHitachiM32rKlinePlan(FlashOperation::kRead, "sub_ecu_hitachi_m32r_kline",
                                                "M32R_512KB_1block", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40});
    transport.QueueError(ErrorKind::kDisconnected, "read fault");
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("read fault")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

const bytes::Bytes& SweepImage()
{
    static const bytes::Bytes kImage(0x80000, 0xa5);
    return kImage;
}

struct Scenario
{
    std::string_view name;
    std::string_view protocol;
    FlashOperation operation;
    void (*script)(ScriptedKlineFlashTransport&, std::uint32_t end);
};

Result<FlashPlan> PlanFor(const Scenario& scenario)
{
    return BuildSubaruHitachiM32rKlinePlan(
        scenario.operation, scenario.protocol, "M32R_512KB_1block",
        scenario.operation == FlashOperation::kRead ? std::nullopt : std::optional<bytes::Bytes>(SweepImage()));
}

void ScriptFallbackRead(ScriptedKlineFlashTransport& transport, std::uint32_t end)
{
    transport.Exchange(Frame({0xbf}));
    transport.QueueNoFrame();
    transport.Exchange(Frame({0xbf}), IdResponse());
    transport.Exchange(Frame({0xb8, 0, 0, 0, 0x75}), bytes::Bytes{0, 0, 0, 0, 0xf8});
    transport.Exchange(Frame({0xbf}), IdResponse());
    ScriptReadChunks(transport, end);
}

void ScriptActiveObkWrite(ScriptedKlineFlashTransport& transport, std::uint32_t end)
{
    transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}), bytes::Bytes{0, 0, 0, 0, 0x74, 0x84});
    ScriptWriteBody(transport, SweepImage(), true, end);
}

void ScriptFallbackWrite(ScriptedKlineFlashTransport& transport, std::uint32_t end)
{
    transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}));
    transport.QueueNoFrame();
    transport.Exchange(Frame({0xbf}), IdResponse());
    transport.Exchange(Frame({0x81}), bytes::Bytes{0, 0, 0, 0, 0xc1});
    transport.Exchange(Frame({0x83, 0x00}), bytes::Bytes{0, 0, 0, 0, 0xc3});
    transport.Exchange(Frame({0x27, 0x01}), bytes::Bytes{0, 0, 0, 0, 0x67, 0x01, 0x12, 0x34, 0x56, 0x78});
    transport.Exchange(Frame({0x27, 0x02, 0x6c, 0xf8, 0x3a, 0x6a}), bytes::Bytes{0, 0, 0, 0, 0x67, 0x02});
    transport.Exchange(Frame({0x10, 0x85, 0x02}), bytes::Bytes{0, 0, 0, 0, 0x50});
    ScriptWriteBody(transport, SweepImage(), true, end);
}

void ScriptRecoveryWrite(ScriptedKlineFlashTransport& transport, std::uint32_t end)
{
    transport.Exchange(Frame({0x81}));
    transport.QueueNoFrame();
    transport.Exchange(Frame({0x81}), bytes::Bytes{0, 0, 0, 0, 0xc1});
    ScriptAuthenticatedTail(transport);
    ScriptWriteBody(transport, SweepImage(), true, end);
}

const std::vector<Scenario>& Scenarios()
{
    static const std::vector<Scenario> kScenarios{
        {"fallback read", "sub_ecu_hitachi_m32r_kline", FlashOperation::kRead, &ScriptFallbackRead},
        {"active OBK write", "sub_ecu_hitachi_m32r_kline", FlashOperation::kWrite, &ScriptActiveObkWrite},
        {"fallback write", "sub_ecu_hitachi_m32r_kline", FlashOperation::kWrite, &ScriptFallbackWrite},
        {"recovery write", "sub_ecu_hitachi_m32r_kline_recovery", FlashOperation::kWrite, &ScriptRecoveryWrite},
    };
    return kScenarios;
}

// Session setup ends well inside these many calls or checks; the chunk loops
// after it repeat one exchange shape. Sweeps script only the chunks below kSweptEnd:
// a run that gets past them never reaches its fault.
constexpr std::size_t kSessionPrefix = 40;
constexpr std::uint32_t kSweptEnd = 64 * 0x80;
constexpr std::uint32_t kRomEnd = 0x80000;

TEST(SubaruHitachiM32rKlineExecutor, EveryEarlyTransportFaultPropagates)
{
    for (const Scenario& scenario : Scenarios())
    {
        SCOPED_TRACE(scenario.name);
        auto plan = PlanFor(scenario);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        for (const KlineFaultOp op :
             {KlineFaultOp::kWrite, KlineFaultOp::kRead, KlineFaultOp::kSetBaud, KlineFaultOp::kHeaderMode})
        {
            SCOPED_TRACE(::testing::Message() << "op " << static_cast<int>(op));
            for (std::size_t k = 1; k <= kSessionPrefix; ++k)
            {
                SCOPED_TRACE(::testing::Message() << "call " << k);
                FaultingKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
                transport.op = op;
                transport.fail_at = k;
                scenario.script(transport, kSweptEnd);
                FakeClock clock;
                ManualCancellationToken cancellation;
                RecordingEventSink events;
                const auto result =
                    SubaruHitachiM32rKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events);
                if (!transport.writes_at_fault.has_value())
                {
                    // Every `op` call of the swept prefix has been failed once.
                    EXPECT_GT(k, 1U);
                    break;
                }
                EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kDisconnected, Eq("injected")));
                EXPECT_EQ(transport.writes_at_fault, std::optional<std::size_t>{transport.WritesConsumed()});
            }
        }
    }
}

TEST(SubaruHitachiM32rKlineExecutor, EveryEarlyCancellationCheckStops)
{
    for (const Scenario& scenario : Scenarios())
    {
        SCOPED_TRACE(scenario.name);
        auto plan = PlanFor(scenario);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        for (std::size_t n = 1; n <= kSessionPrefix; ++n)
        {
            SCOPED_TRACE(n);
            ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
            scenario.script(transport, kSweptEnd);
            FakeClock clock;
            FakeCancellationToken cancellation;
            std::size_t seen = 0;
            std::optional<std::size_t> writes_at_cancel;
            cancellation.SetPredicate(
                [&]
                {
                    if (++seen == n)
                    {
                        writes_at_cancel = transport.WritesConsumed();
                    }
                    return seen >= n;
                });
            RecordingEventSink events;
            EXPECT_THAT(SubaruHitachiM32rKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                        fastecu::testing::IsErr(ErrorKind::kCancelled));
            EXPECT_EQ(writes_at_cancel, std::optional<std::size_t>{transport.WritesConsumed()});
        }
    }
}

TEST(SubaruHitachiM32rKlineExecutor, EverySleepFailureInAWritePropagates)
{
    for (const Scenario& scenario : Scenarios())
    {
        SCOPED_TRACE(scenario.name);
        auto plan = PlanFor(scenario);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        std::size_t count = 0;
        {
            ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
            scenario.script(transport, kRomEnd);
            FaultingClock clock;
            ManualCancellationToken cancellation;
            RecordingEventSink events;
            ASSERT_THAT(SubaruHitachiM32rKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                        fastecu::testing::IsOk());
            count = clock.sleeps;
        }
        for (std::size_t k = 1; k <= count; ++k)
        {
            SCOPED_TRACE(::testing::Message() << "sleep " << k);
            ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
            scenario.script(transport, kRomEnd);
            FaultingClock clock;
            clock.fail_at = k;
            ManualCancellationToken cancellation;
            RecordingEventSink events;
            EXPECT_THAT(SubaruHitachiM32rKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                        fastecu::testing::IsErrWith(ErrorKind::kDisconnected, Eq("sleep failed")));
        }
    }
}

TEST(SubaruHitachiM32rKlineExecutor, FinalChecksumMustAnswer)
{
    struct Case
    {
        std::string_view name;
        void (*reply)(ScriptedKlineFlashTransport&);
        ErrorKind kind;
        std::string_view detail;
    };
    const std::vector<Case> cases{
        {"no frame", [](ScriptedKlineFlashTransport& t) { t.QueueNoFrame(); }, ErrorKind::kTimeout,
         "no final checksum response"},
        {"empty frame", [](ScriptedKlineFlashTransport& t) { t.QueueRead(bytes::Bytes{}); }, ErrorKind::kTimeout,
         "no final checksum response"},
        {"read error",
         [](ScriptedKlineFlashTransport& t) { t.QueueError(ErrorKind::kDisconnected, "checksum read failed"); },
         ErrorKind::kDisconnected, "checksum read failed"},
    };
    const Scenario& scenario = Scenarios()[1];
    auto plan = PlanFor(scenario);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.name);
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}), bytes::Bytes{0, 0, 0, 0, 0x74, 0x84});
        ScriptWriteBody(transport, SweepImage(), /*checksum=*/false);
        transport.ExpectWrite(Frame({0x31, 0x01, 0x02}));
        c.reply(transport);
        FakeClock clock;
        ManualCancellationToken cancellation;
        RecordingEventSink events;
        EXPECT_THAT(SubaruHitachiM32rKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErrWith(c.kind, Eq(c.detail)));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruHitachiM32rKlineExecutor, ShortEcuIdStopsTheReadAndWriteFallbacks)
{
    const bytes::Bytes short_id{0x80, 0xf0, 0x10, 0x08, 0xff, 0, 0, 0, 0x12, 0x34, 0x56, 0x78};
    {
        SCOPED_TRACE("read");
        auto plan = PlanFor(Scenarios()[0]);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        transport.Exchange(Frame({0xbf}));
        transport.QueueNoFrame();
        transport.Exchange(Frame({0xbf}), short_id);
        FakeClock clock;
        ManualCancellationToken cancellation;
        RecordingEventSink events;
        EXPECT_THAT(SubaruHitachiM32rKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErrWith(ErrorKind::kBadResponse, Eq("ECU ID response is too short")));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
    {
        SCOPED_TRACE("write");
        auto plan = PlanFor(Scenarios()[2]);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        transport.Exchange(Frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}));
        transport.QueueNoFrame();
        transport.Exchange(Frame({0xbf}), short_id);
        FakeClock clock;
        ManualCancellationToken cancellation;
        RecordingEventSink events;
        EXPECT_THAT(SubaruHitachiM32rKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErrWith(ErrorKind::kBadResponse, Eq("ECU ID response is too short")));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruHitachiM32rKlineExecutor, BadSeedRepliesStopBeforeTheKey)
{
    struct Case
    {
        bytes::Bytes reply;
        std::string_view detail;
    };
    const std::vector<Case> cases{
        {bytes::Bytes{0, 0, 0, 0, 0x67, 0x02, 0x12, 0x34, 0x56, 0x78}, "wrong response from ECU"},
        {bytes::Bytes{0, 0, 0, 0, 0x67, 0x01, 0x12, 0x34, 0x56}, "seed response is too short"},
    };
    auto plan = PlanFor(Scenarios()[3]);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.reply.size());
        ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};
        transport.Exchange(Frame({0x81}), bytes::Bytes{0, 0, 0, 0, 0xc1});
        transport.Exchange(Frame({0x83, 0x00}), bytes::Bytes{0, 0, 0, 0, 0xc3});
        transport.Exchange(Frame({0x27, 0x01}), c.reply);
        FakeClock clock;
        ManualCancellationToken cancellation;
        RecordingEventSink events;
        EXPECT_THAT(SubaruHitachiM32rKlineExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErrWith(ErrorKind::kBadResponse, Eq(c.detail)));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}
} // namespace
