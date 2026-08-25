#include "src/platform/desktop/common/logging/legacy_logging_protocol_factory.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

namespace fastecu::desktop::logging
{

class LegacyLoggingProtocolFactoryTestAccess
{
  public:
    using ProtocolResult = LegacyLoggingProtocolFactory::ProtocolResult;
    using ProtocolBuilder = LegacyLoggingProtocolFactory::ProtocolBuilder;
    using SsmProtocolBuilder = LegacyLoggingProtocolFactory::SsmProtocolBuilder;

    static LegacyLoggingProtocolFactory make(
        ProtocolBuilder mut_dma_builder, ProtocolBuilder cdbg_builder, SsmProtocolBuilder ssm_builder,
        CdbgSerialSetupActions cdbg_serial_setup = successfulCdbgSetup(),
        std::function<bool()> open_cdbg_port = []() { return true; },
        std::function<bool()> cdbg_port_is_open = []() { return true; },
        std::function<bool()> target_is_ecu = []() { return true; },
        std::function<bool()> use_openport2_adapter = []() { return false; })
    {
        return LegacyLoggingProtocolFactory(
            {
                .mut_dma_builder = std::move(mut_dma_builder),
                .cdbg_builder = std::move(cdbg_builder),
                .ssm_builder = std::move(ssm_builder),
                .cdbg_serial_setup = std::move(cdbg_serial_setup),
                .open_cdbg_port = std::move(open_cdbg_port),
                .cdbg_port_is_open = std::move(cdbg_port_is_open),
                .target_is_ecu = std::move(target_is_ecu),
                .use_openport2_adapter = std::move(use_openport2_adapter),
            },
            LegacyLoggingProtocolFactory::TestingTag{});
    }

    static CdbgSerialSetupActions successfulCdbgSetup()
    {
        return {
            .disable_iso14230 = []() { return true; },
            .disable_iso14230_header = []() { return true; },
            .enable_raw_can = []() { return true; },
            .disable_iso15765 = []() { return true; },
            .select_11_bit_ids = []() { return true; },
            .select_500k_baud = []() { return true; },
            .select_reply_id = []() { return true; },
        };
    }
};

namespace
{

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

class StubProtocol final : public fastecu::logging::LoggingProtocol
{
  public:
    fastecu::Status start(const fastecu::ICancellationToken&) override
    {
        return {};
    }

    fastecu::Result<fastecu::logging::PollData> poll(int, const fastecu::ICancellationToken&) override
    {
        return fastecu::logging::PollData{};
    }

    fastecu::Status stop() override
    {
        return {};
    }
};

using ProtocolResult = LegacyLoggingProtocolFactoryTestAccess::ProtocolResult;
using ProtocolBuilder = LegacyLoggingProtocolFactoryTestAccess::ProtocolBuilder;
using SsmProtocolBuilder = LegacyLoggingProtocolFactoryTestAccess::SsmProtocolBuilder;

ProtocolResult make_protocol()
{
    return std::unique_ptr<fastecu::logging::LoggingProtocol>(std::make_unique<StubProtocol>());
}

ProtocolBuilder successful_builder()
{
    return [](const std::vector<fastecu::logging::LoggingChannel>&) { return make_protocol(); };
}

SsmProtocolBuilder successful_ssm_builder()
{
    return [](const std::vector<fastecu::logging::LoggingChannel>&, const std::vector<std::size_t>&, bool, bool)
    { return make_protocol(); };
}

std::vector<fastecu::logging::LoggingChannel> sample_channels()
{
    return {
        {.id = "rpm", .address = 0x1234, .length = 2},
        {.id = "coolant", .address = 0xabcdef, .length = 1},
    };
}

void expect_channel_identity(const std::vector<fastecu::logging::LoggingChannel>& channels)
{
    ASSERT_EQ(channels.size(), 2U);
    EXPECT_EQ(channels[0].id, "rpm");
    EXPECT_EQ(channels[0].address, 0x1234U);
    EXPECT_EQ(channels[1].id, "coolant");
    EXPECT_EQ(channels[1].address, 0xabcdefU);
}

struct BuilderRecording
{
    int mut_dma_calls = 0;
    int cdbg_calls = 0;
    int ssm_calls = 0;
    int target_is_ecu_calls = 0;
    int use_openport2_adapter_calls = 0;
    std::vector<fastecu::logging::LoggingChannel> channels;
    std::vector<std::size_t> ssm_response_offsets;
    std::optional<bool> target_is_ecu;
    std::optional<bool> use_openport2_adapter;
};

LegacyLoggingProtocolFactory recording_factory(BuilderRecording& recording)
{
    return LegacyLoggingProtocolFactoryTestAccess::make(
        [&recording](const std::vector<fastecu::logging::LoggingChannel>& channels)
        {
            ++recording.mut_dma_calls;
            recording.channels = channels;
            return make_protocol();
        },
        [&recording](const std::vector<fastecu::logging::LoggingChannel>& channels)
        {
            ++recording.cdbg_calls;
            recording.channels = channels;
            return make_protocol();
        },
        [&recording](const std::vector<fastecu::logging::LoggingChannel>& channels,
                     const std::vector<std::size_t>& response_offsets, bool target_is_ecu, bool use_openport2_adapter)
        {
            ++recording.ssm_calls;
            recording.channels = channels;
            recording.ssm_response_offsets = response_offsets;
            recording.target_is_ecu = target_is_ecu;
            recording.use_openport2_adapter = use_openport2_adapter;
            return make_protocol();
        },
        LegacyLoggingProtocolFactoryTestAccess::successfulCdbgSetup(), []() { return true; }, []() { return true; },
        [&recording]()
        {
            ++recording.target_is_ecu_calls;
            return false;
        },
        [&recording]()
        {
            ++recording.use_openport2_adapter_calls;
            return true;
        });
}

TEST(LegacyLoggingProtocolFactoryTest, MutDmaDispatchesOnlyToMutDmaBuilderWithUnchangedChannels)
{
    BuilderRecording recording;
    auto factory = recording_factory(recording);

    auto result = factory.create({
        .protocol = fastecu::logging::LoggingProtocolId::MutDma,
        .channels = sample_channels(),
        .ssm_response_offsets = {3, 7},
    });

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->get(), nullptr);
    EXPECT_EQ(recording.mut_dma_calls, 1);
    EXPECT_EQ(recording.cdbg_calls, 0);
    EXPECT_EQ(recording.ssm_calls, 0);
    EXPECT_EQ(recording.target_is_ecu_calls, 0);
    EXPECT_EQ(recording.use_openport2_adapter_calls, 0);
    expect_channel_identity(recording.channels);
    EXPECT_TRUE(recording.ssm_response_offsets.empty());
}

TEST(LegacyLoggingProtocolFactoryTest, CdbgDispatchesOnlyToCdbgBuilderWithUnchangedChannels)
{
    BuilderRecording recording;
    auto factory = recording_factory(recording);

    auto result = factory.create({
        .protocol = fastecu::logging::LoggingProtocolId::Cdbg,
        .channels = sample_channels(),
        .ssm_response_offsets = {3, 7},
    });

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->get(), nullptr);
    EXPECT_EQ(recording.mut_dma_calls, 0);
    EXPECT_EQ(recording.cdbg_calls, 1);
    EXPECT_EQ(recording.ssm_calls, 0);
    EXPECT_EQ(recording.target_is_ecu_calls, 0);
    EXPECT_EQ(recording.use_openport2_adapter_calls, 0);
    expect_channel_identity(recording.channels);
    EXPECT_TRUE(recording.ssm_response_offsets.empty());
}

TEST(LegacyLoggingProtocolFactoryTest, SsmDispatchesOnlyToSsmBuilderWithOffsetsAndSelections)
{
    BuilderRecording recording;
    auto factory = recording_factory(recording);

    auto result = factory.create({
        .protocol = fastecu::logging::LoggingProtocolId::Ssm,
        .channels = sample_channels(),
        .ssm_response_offsets = {3, 7},
    });

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->get(), nullptr);
    EXPECT_EQ(recording.mut_dma_calls, 0);
    EXPECT_EQ(recording.cdbg_calls, 0);
    EXPECT_EQ(recording.ssm_calls, 1);
    EXPECT_EQ(recording.target_is_ecu_calls, 1);
    EXPECT_EQ(recording.use_openport2_adapter_calls, 1);
    expect_channel_identity(recording.channels);
    EXPECT_THAT(recording.ssm_response_offsets, ElementsAre(3U, 7U));
    EXPECT_EQ(recording.target_is_ecu, false);
    EXPECT_EQ(recording.use_openport2_adapter, true);
}

TEST(LegacyLoggingProtocolFactoryTest, PreservesChosenBuilderError)
{
    auto factory = LegacyLoggingProtocolFactoryTestAccess::make(
        [](const std::vector<fastecu::logging::LoggingChannel>&) -> ProtocolResult
        { return fastecu::fail(fastecu::ErrorKind::Timeout, "recorded builder error"); }, successful_builder(),
        successful_ssm_builder());

    auto result = factory.create({.protocol = fastecu::logging::LoggingProtocolId::MutDma});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), (fastecu::Error{fastecu::ErrorKind::Timeout, "recorded builder error"}));
}

TEST(LegacyLoggingProtocolFactoryTest, RejectsSuccessfulNullProtocol)
{
    auto factory = LegacyLoggingProtocolFactoryTestAccess::make(
        [](const std::vector<fastecu::logging::LoggingChannel>&) -> ProtocolResult
        { return std::unique_ptr<fastecu::logging::LoggingProtocol>{}; }, successful_builder(),
        successful_ssm_builder());

    auto result = factory.create({.protocol = fastecu::logging::LoggingProtocolId::MutDma});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), (fastecu::Error{fastecu::ErrorKind::Internal, "legacy protocol builder returned null"}));
}

TEST(LegacyLoggingProtocolFactoryTest, NormalizesStandardBuilderException)
{
    auto factory = LegacyLoggingProtocolFactoryTestAccess::make(
        [](const std::vector<fastecu::logging::LoggingChannel>&) -> ProtocolResult
        { throw std::runtime_error("builder failed"); }, successful_builder(), successful_ssm_builder());

    auto result = factory.create({.protocol = fastecu::logging::LoggingProtocolId::MutDma});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), (fastecu::Error{fastecu::ErrorKind::Internal, "builder failed"}));
}

TEST(LegacyLoggingProtocolFactoryTest, NormalizesUnknownBuilderException)
{
    auto factory = LegacyLoggingProtocolFactoryTestAccess::make(
        [](const std::vector<fastecu::logging::LoggingChannel>&) -> ProtocolResult { throw 42; }, successful_builder(),
        successful_ssm_builder());

    auto result = factory.create({.protocol = fastecu::logging::LoggingProtocolId::MutDma});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(),
              (fastecu::Error{fastecu::ErrorKind::Internal, "legacy protocol builder threw an unknown exception"}));
}

CdbgSerialSetupActions recording_cdbg_setup(std::vector<int>& events, std::optional<int> failing_action = std::nullopt)
{
    const auto action = [&events, failing_action](int index)
    {
        return [&events, failing_action, index]()
        {
            events.push_back(index);
            return failing_action != index;
        };
    };
    return {
        .disable_iso14230 = action(0),
        .disable_iso14230_header = action(1),
        .enable_raw_can = action(2),
        .disable_iso15765 = action(3),
        .select_11_bit_ids = action(4),
        .select_500k_baud = action(5),
        .select_reply_id = action(6),
    };
}

TEST(LegacyLoggingProtocolFactoryTest, CdbgConfiguresThenOpensChecksAndBuildsInOrder)
{
    std::vector<int> events;
    auto factory = LegacyLoggingProtocolFactoryTestAccess::make(
        successful_builder(),
        [&events](const std::vector<fastecu::logging::LoggingChannel>&)
        {
            events.push_back(9);
            return make_protocol();
        },
        successful_ssm_builder(), recording_cdbg_setup(events),
        [&events]()
        {
            events.push_back(7);
            return true;
        },
        [&events]()
        {
            events.push_back(8);
            return true;
        });

    auto result = factory.create({.protocol = fastecu::logging::LoggingProtocolId::Cdbg});

    ASSERT_TRUE(result.has_value());
    EXPECT_THAT(events, ElementsAre(0, 1, 2, 3, 4, 5, 6, 7, 8, 9));
}

TEST(LegacyLoggingProtocolFactoryTest, CdbgSetupFailureStopsAllLaterActions)
{
    for (int failing_action = 0; failing_action < 7; ++failing_action)
    {
        SCOPED_TRACE(failing_action);
        std::vector<int> events;
        auto factory = LegacyLoggingProtocolFactoryTestAccess::make(
            successful_builder(),
            [&events](const std::vector<fastecu::logging::LoggingChannel>&)
            {
                events.push_back(9);
                return make_protocol();
            },
            successful_ssm_builder(), recording_cdbg_setup(events, failing_action),
            [&events]()
            {
                events.push_back(7);
                return true;
            },
            [&events]()
            {
                events.push_back(8);
                return true;
            });

        auto result = factory.create({.protocol = fastecu::logging::LoggingProtocolId::Cdbg});

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
        std::vector<int> expected_events;
        for (int index = 0; index <= failing_action; ++index)
        {
            expected_events.push_back(index);
        }
        EXPECT_THAT(events, ElementsAreArray(expected_events));
    }
}

TEST(LegacyLoggingProtocolFactoryTest, CdbgOpenFailureDoesNotCheckOrBuildProtocol)
{
    std::vector<int> events;
    auto factory = LegacyLoggingProtocolFactoryTestAccess::make(
        successful_builder(),
        [&events](const std::vector<fastecu::logging::LoggingChannel>&)
        {
            events.push_back(9);
            return make_protocol();
        },
        successful_ssm_builder(), recording_cdbg_setup(events),
        [&events]()
        {
            events.push_back(7);
            return false;
        },
        [&events]()
        {
            events.push_back(8);
            return true;
        });

    auto result = factory.create({.protocol = fastecu::logging::LoggingProtocolId::Cdbg});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(),
              (fastecu::Error{fastecu::ErrorKind::Disconnected, "unable to open CAN adapter for CDBG logging"}));
    EXPECT_THAT(events, ElementsAre(0, 1, 2, 3, 4, 5, 6, 7));
}

TEST(LegacyLoggingProtocolFactoryTest, CdbgClosedPortDoesNotBuildProtocol)
{
    std::vector<int> events;
    auto factory = LegacyLoggingProtocolFactoryTestAccess::make(
        successful_builder(),
        [&events](const std::vector<fastecu::logging::LoggingChannel>&)
        {
            events.push_back(9);
            return make_protocol();
        },
        successful_ssm_builder(), recording_cdbg_setup(events),
        [&events]()
        {
            events.push_back(7);
            return true;
        },
        [&events]()
        {
            events.push_back(8);
            return false;
        });

    auto result = factory.create({.protocol = fastecu::logging::LoggingProtocolId::Cdbg});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(),
              (fastecu::Error{fastecu::ErrorKind::Disconnected, "unable to open CAN adapter for CDBG logging"}));
    EXPECT_THAT(events, ElementsAre(0, 1, 2, 3, 4, 5, 6, 7, 8));
}

} // namespace
} // namespace fastecu::desktop::logging
