#include "src/backend/flash/testing/scripted_flash_transport_source.h"

#include <gmock/gmock-matchers.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <utility>

using ::testing::Eq;

namespace fastecu::flash
{
namespace
{

void ExpectUntouched(const ScriptedKlineFlashTransport& transport)
{
    EXPECT_FALSE(transport.IsOpen());
    EXPECT_THAT(transport.last_config, Eq(std::nullopt));
    EXPECT_THAT(transport.lifecycle_calls, ::testing::SizeIs(0));
}

void ExpectUntouched(const ScriptedCanFlashTransport& transport)
{
    EXPECT_FALSE(transport.IsOpen());
    EXPECT_THAT(transport.last_config, Eq(std::nullopt));
    EXPECT_THAT(transport.lifecycle_calls, ::testing::SizeIs(0));
}

void ExpectUntouched(const ScriptedMixedCanFlashTransport& transport)
{
    EXPECT_THAT(transport.last_config, Eq(std::nullopt));
    EXPECT_EQ(transport.ConfigureCallCount(), 0);
    EXPECT_EQ(transport.OpenCallCount(), 0);
    EXPECT_EQ(transport.CloseCallCount(), 0);
    EXPECT_EQ(transport.reset_connection_call_count, 0);
    // No mode transition leaves the default kUnconfigured mode in place.
    EXPECT_THAT(transport.ModeChanges(), ::testing::SizeIs(0));
}

TEST(ScriptedFlashTransportSource, defaultsAreFreshAndUnopened)
{
    ScriptedFlashTransportSource source;
    auto kline1 = source.MakeKline();
    auto can1 = source.MakeCan();
    auto mixed1 = source.MakeMixedCan();
    auto kline2 = source.MakeKline();
    auto can2 = source.MakeCan();
    auto mixed2 = source.MakeMixedCan();
    ASSERT_NE(kline1, nullptr);
    ASSERT_NE(kline2, nullptr);
    ASSERT_NE(can1, nullptr);
    ASSERT_NE(can2, nullptr);
    ASSERT_NE(mixed1, nullptr);
    ASSERT_NE(mixed2, nullptr);
    EXPECT_NE(kline1.get(), kline2.get());
    EXPECT_NE(can1.get(), can2.get());
    EXPECT_NE(mixed1.get(), mixed2.get());
    ExpectUntouched(*static_cast<ScriptedKlineFlashTransport *>(kline1.get()));
    ExpectUntouched(*static_cast<ScriptedKlineFlashTransport *>(kline2.get()));
    ExpectUntouched(*static_cast<ScriptedCanFlashTransport *>(can1.get()));
    ExpectUntouched(*static_cast<ScriptedCanFlashTransport *>(can2.get()));
    ExpectUntouched(*static_cast<ScriptedMixedCanFlashTransport *>(mixed1.get()));
    ExpectUntouched(*static_cast<ScriptedMixedCanFlashTransport *>(mixed2.get()));
    EXPECT_THAT(source.RequestedKinds(),
                ::testing::ElementsAre(TransportKind::kKline, TransportKind::kCanIso15765,
                                       TransportKind::kCanRawIso15765, TransportKind::kKline,
                                       TransportKind::kCanIso15765, TransportKind::kCanRawIso15765));
}

TEST(ScriptedFlashTransportSource, preloadsAreFifoAndIndependentAcrossKinds)
{
    ScriptedFlashTransportSource source;
    auto first_kline = std::make_unique<ScriptedKlineFlashTransport>();
    auto second_kline = std::make_unique<ScriptedKlineFlashTransport>();
    auto first_can = std::make_unique<ScriptedCanFlashTransport>();
    auto second_can = std::make_unique<ScriptedCanFlashTransport>();
    auto first_mixed = std::make_unique<ScriptedMixedCanFlashTransport>();
    auto second_mixed = std::make_unique<ScriptedMixedCanFlashTransport>();
    auto *first_kline_observer = first_kline.get();
    auto *second_kline_observer = second_kline.get();
    auto *first_can_observer = first_can.get();
    auto *second_can_observer = second_can.get();
    auto *first_mixed_observer = first_mixed.get();
    auto *second_mixed_observer = second_mixed.get();
    source.QueueKline(std::move(first_kline));
    source.QueueCan(std::move(first_can));
    source.QueueMixedCan(std::move(first_mixed));
    source.QueueCan(std::move(second_can));
    source.QueueMixedCan(std::move(second_mixed));
    source.QueueKline(std::move(second_kline));

    auto kline1 = source.MakeKline();
    auto can1 = source.MakeCan();
    auto mixed1 = source.MakeMixedCan();
    auto can2 = source.MakeCan();
    auto can3 = source.MakeCan();
    auto can4 = source.MakeCan();
    auto kline2 = source.MakeKline();
    auto mixed2 = source.MakeMixedCan();
    auto kline3 = source.MakeKline();
    auto mixed3 = source.MakeMixedCan();
    auto kline4 = source.MakeKline();
    auto mixed4 = source.MakeMixedCan();
    EXPECT_EQ(kline1.get(), first_kline_observer);
    EXPECT_EQ(kline2.get(), second_kline_observer);
    EXPECT_EQ(can1.get(), first_can_observer);
    EXPECT_EQ(can2.get(), second_can_observer);
    EXPECT_EQ(mixed1.get(), first_mixed_observer);
    EXPECT_EQ(mixed2.get(), second_mixed_observer);
    ExpectUntouched(*first_kline_observer);
    ExpectUntouched(*second_kline_observer);
    ExpectUntouched(*first_can_observer);
    ExpectUntouched(*second_can_observer);
    ExpectUntouched(*first_mixed_observer);
    ExpectUntouched(*second_mixed_observer);
    ASSERT_NE(kline3, nullptr);
    ASSERT_NE(kline4, nullptr);
    ASSERT_NE(can3, nullptr);
    ASSERT_NE(can4, nullptr);
    ASSERT_NE(mixed3, nullptr);
    ASSERT_NE(mixed4, nullptr);
    EXPECT_NE(kline3.get(), kline4.get());
    EXPECT_NE(can3.get(), can4.get());
    EXPECT_NE(mixed3.get(), mixed4.get());
    ExpectUntouched(*static_cast<ScriptedKlineFlashTransport *>(kline3.get()));
    ExpectUntouched(*static_cast<ScriptedKlineFlashTransport *>(kline4.get()));
    ExpectUntouched(*static_cast<ScriptedCanFlashTransport *>(can3.get()));
    ExpectUntouched(*static_cast<ScriptedCanFlashTransport *>(can4.get()));
    ExpectUntouched(*static_cast<ScriptedMixedCanFlashTransport *>(mixed3.get()));
    ExpectUntouched(*static_cast<ScriptedMixedCanFlashTransport *>(mixed4.get()));
    EXPECT_THAT(
        source.RequestedKinds(),
        ::testing::ElementsAre(TransportKind::kKline, TransportKind::kCanIso15765, TransportKind::kCanRawIso15765,
                               TransportKind::kCanIso15765, TransportKind::kCanIso15765, TransportKind::kCanIso15765,
                               TransportKind::kKline, TransportKind::kCanRawIso15765, TransportKind::kKline,
                               TransportKind::kCanRawIso15765, TransportKind::kKline, TransportKind::kCanRawIso15765));
    EXPECT_FALSE(source.SuppliesProgrammingVoltage());
}

TEST(ScriptedFlashTransportSource, programmingVoltageIsAnIndependentConstructorFlag)
{
    ScriptedFlashTransportSource source;
    EXPECT_FALSE(source.SuppliesProgrammingVoltage());
    EXPECT_THAT(source.RequestedKinds(), ::testing::SizeIs(0));
    ScriptedFlashTransportSource powered(true);
    EXPECT_TRUE(powered.SuppliesProgrammingVoltage());
    EXPECT_THAT(powered.RequestedKinds(), ::testing::SizeIs(0));
    powered.QueueKline(std::make_unique<ScriptedKlineFlashTransport>());
    auto kline = powered.MakeKline();
    auto can = powered.MakeCan();
    auto mixed = powered.MakeMixedCan();
    ASSERT_NE(kline, nullptr);
    ASSERT_NE(can, nullptr);
    ASSERT_NE(mixed, nullptr);
    EXPECT_TRUE(powered.SuppliesProgrammingVoltage());
    EXPECT_THAT(powered.RequestedKinds(), ::testing::ElementsAre(TransportKind::kKline, TransportKind::kCanIso15765,
                                                                 TransportKind::kCanRawIso15765));
}

} // namespace
} // namespace fastecu::flash
