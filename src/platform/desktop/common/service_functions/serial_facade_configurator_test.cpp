#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <string>
#include "src/platform/desktop/common/service_functions/serial_facade_configurator.h"

#include <gtest/gtest.h>
#include <QCoreApplication>

#include <gmock/gmock.h>

#include <memory>
#include <stdexcept>

#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

using fastecu::ErrorKind;
using fastecu::service_functions::SerialPortActionsConfigurator;
using fastecu::service_functions::SsmTransportConfig;

namespace
{

SsmTransportConfig klineConfig()
{
    return SsmTransportConfig{
        .framing = SsmTransportConfig::Framing::kKline14230,
        .bitrate_or_baud = 4800,
        .request_id = 0,
        .response_id = 0,
        .tester_id = 0xf0,
        .target_id = 0x18,
        .add_iso14230_header = false,
    };
}

struct Harness
{
    Harness()
    {
        serial = std::make_unique<SerialPortActions>(
            [this]() -> SerialBackend *
            {
                fake = new NiceFakeBackend;
                return fake;
            });
        serial->set_add_ssm_header(false); // start the facade's backend thread
        EXPECT_CALL(*fake, open_serial_port()).WillRepeatedly(::testing::Return(QStringLiteral("fake-port")));
        EXPECT_CALL(*fake, is_serial_port_open()).WillRepeatedly(::testing::Return(true));
        configurator = std::make_unique<SerialPortActionsConfigurator>(serial.get());
    }

    FakeBackend *fake = nullptr;
    std::unique_ptr<SerialPortActions> serial;
    std::unique_ptr<SerialPortActionsConfigurator> configurator;
};

} // namespace

TEST(SerialFacadeConfiguratorTest, isoConfigurationClearsAStaleKlineHeaderAndUsesTheRequiredOrder)
{
    Harness harness;
    ASSERT_TRUE(harness.serial->set_add_iso14230_header(true));

    ::testing::InSequence sequence;
    EXPECT_CALL(*harness.fake, reset_connection()).WillOnce(::testing::Return());
    EXPECT_CALL(*harness.fake, set_is_iso14230_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, set_is_can_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, set_is_iso15765_connection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, set_is_29_bit_id(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, set_add_iso14230_header(false)).WillOnce(::testing::DoDefault());
    EXPECT_CALL(*harness.fake, set_can_speed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, set_iso15765_source_address(2017)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, set_iso15765_destination_address(2025)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, set_can_source_address(2017)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, set_can_destination_address(2025)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, open_serial_port()).WillOnce(::testing::Return(QStringLiteral("fake-port")));
    EXPECT_CALL(*harness.fake, is_serial_port_open()).WillOnce(::testing::Return(true));

    const auto result = harness.configurator->apply(SsmTransportConfig{});

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(harness.serial->get_add_iso14230_header(), false);
}

TEST(SerialFacadeConfiguratorTest, klineConfigurationPreservesLegacyOpenBaudHeaderOrder)
{
    Harness harness;

    ::testing::InSequence sequence;
    EXPECT_CALL(*harness.fake, reset_connection()).WillOnce(::testing::Return());
    EXPECT_CALL(*harness.fake, set_is_can_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, set_is_iso15765_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, set_is_iso14230_connection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, open_serial_port()).WillOnce(::testing::Return(QStringLiteral("fake-port")));
    EXPECT_CALL(*harness.fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, change_port_speed(QStringLiteral("4800"))).WillOnce(::testing::Return(kSerialSuccess));
    EXPECT_CALL(*harness.fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*harness.fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(true));

    const auto result = harness.configurator->apply(klineConfig());

    ASSERT_TRUE(result.has_value());
}

TEST(SerialFacadeConfiguratorTest, nullFacadeIsDisconnected)
{
    SerialPortActionsConfigurator configurator{nullptr};

    const auto result = configurator.apply(SsmTransportConfig{});

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

TEST(SerialFacadeConfiguratorTest, anEmptyOpenResultIsDisconnectedEvenWithAStaleOpenFlag)
{
    Harness harness;
    EXPECT_CALL(*harness.fake, open_serial_port()).WillOnce(::testing::Return(QString{}));

    const auto result = harness.configurator->apply(SsmTransportConfig{});

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

TEST(SerialFacadeConfiguratorTest, aPortThatIsNotOpenAfterOpenIsDisconnected)
{
    Harness harness;
    EXPECT_CALL(*harness.fake, is_serial_port_open()).WillOnce(::testing::Return(false));
    // A port that never opened must not be driven to a new baud rate.
    EXPECT_CALL(*harness.fake, change_port_speed(::testing::_)).Times(0);

    const auto result = harness.configurator->apply(klineConfig());

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

struct EachBooleanSetterFailureIsInvalidConfigCase
{
    std::string name;
    int setter;
};
class EachBooleanSetterFailureIsInvalidConfigParameters
    : public ::testing::Test,
      public ::testing::WithParamInterface<EachBooleanSetterFailureIsInvalidConfigCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, EachBooleanSetterFailureIsInvalidConfigParameters,
    ::testing::Values(EachBooleanSetterFailureIsInvalidConfigCase{"set_is_iso14230_connection", 0},
                      EachBooleanSetterFailureIsInvalidConfigCase{"set_is_can_connection", 1},
                      EachBooleanSetterFailureIsInvalidConfigCase{"set_is_iso15765_connection", 2},
                      EachBooleanSetterFailureIsInvalidConfigCase{"set_is_29_bit_id", 3},
                      EachBooleanSetterFailureIsInvalidConfigCase{"set_add_iso14230_header", 4},
                      EachBooleanSetterFailureIsInvalidConfigCase{"set_can_speed", 5},
                      EachBooleanSetterFailureIsInvalidConfigCase{"set_iso15765_source_address", 6},
                      EachBooleanSetterFailureIsInvalidConfigCase{"set_iso15765_destination_address", 7},
                      EachBooleanSetterFailureIsInvalidConfigCase{"set_can_source_address", 8},
                      EachBooleanSetterFailureIsInvalidConfigCase{"set_can_destination_address", 9}),
    [](const ::testing::TestParamInfo<EachBooleanSetterFailureIsInvalidConfigCase>& info) { return info.param.name; });

TEST_P(EachBooleanSetterFailureIsInvalidConfigParameters, eachBooleanSetterFailureIsInvalidConfig)
{
    const int setter = GetParam().setter;
    Harness harness;
    switch (setter)
    {
    case 0:
        EXPECT_CALL(*harness.fake, set_is_iso14230_connection(false)).WillOnce(::testing::Return(false));
        break;
    case 1:
        EXPECT_CALL(*harness.fake, set_is_can_connection(false)).WillOnce(::testing::Return(false));
        break;
    case 2:
        EXPECT_CALL(*harness.fake, set_is_iso15765_connection(true)).WillOnce(::testing::Return(false));
        break;
    case 3:
        EXPECT_CALL(*harness.fake, set_is_29_bit_id(false)).WillOnce(::testing::Return(false));
        break;
    case 4:
        EXPECT_CALL(*harness.fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(false));
        break;
    case 5:
        EXPECT_CALL(*harness.fake, set_can_speed(::testing::_)).WillOnce(::testing::Return(false));
        break;
    case 6:
        EXPECT_CALL(*harness.fake, set_iso15765_source_address(::testing::_)).WillOnce(::testing::Return(false));
        break;
    case 7:
        EXPECT_CALL(*harness.fake, set_iso15765_destination_address(::testing::_)).WillOnce(::testing::Return(false));
        break;
    case 8:
        EXPECT_CALL(*harness.fake, set_can_source_address(::testing::_)).WillOnce(::testing::Return(false));
        break;
    case 9:
        EXPECT_CALL(*harness.fake, set_can_destination_address(::testing::_)).WillOnce(::testing::Return(false));
        break;
    default:
        FAIL() << "unexpected configuration-setter index";
    }

    const auto result = harness.configurator->apply(SsmTransportConfig{});

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInvalidConfig);
}

TEST(SerialFacadeConfiguratorTest, aKlineHeaderSetterFailureIsInvalidConfig)
{
    Harness harness;
    EXPECT_CALL(*harness.fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(false));

    const auto result = harness.configurator->apply(klineConfig());

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInvalidConfig);
}

TEST(SerialFacadeConfiguratorTest, aSetterExceptionBecomesInternalStatus)
{
    Harness harness;
    EXPECT_CALL(*harness.fake, set_is_iso14230_connection(false))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend config-setter failure")));

    const auto result = harness.configurator->apply(SsmTransportConfig{});

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
}

TEST(SerialFacadeConfiguratorTest, anOpenExceptionBecomesInternalStatus)
{
    Harness harness;
    EXPECT_CALL(*harness.fake, open_serial_port())
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend open failure")));

    const auto result = harness.configurator->apply(SsmTransportConfig{});

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
}

TEST(SerialFacadeConfiguratorTest, aRejectedBaudChangeIsInternal)
{
    Harness harness;
    EXPECT_CALL(*harness.fake, change_port_speed(QStringLiteral("4800"))).WillOnce(::testing::Return(kSerialError));

    const auto result = harness.configurator->apply(klineConfig());

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
}

TEST(SerialFacadeConfiguratorTest, aPortDropDuringRejectedBaudChangeIsDisconnected)
{
    Harness harness;
    EXPECT_CALL(*harness.fake, is_serial_port_open())
        .WillOnce(::testing::Return(true))
        .WillOnce(::testing::Return(false));
    EXPECT_CALL(*harness.fake, change_port_speed(QStringLiteral("4800"))).WillOnce(::testing::Return(kSerialError));

    const auto result = harness.configurator->apply(klineConfig());

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

TEST(SerialFacadeConfiguratorTest, aStandardFacadeExceptionBecomesInternalStatus)
{
    Harness harness;
    EXPECT_CALL(*harness.fake, reset_connection())
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend reset failure")));

    try
    {
        const auto result = harness.configurator->apply(SsmTransportConfig{});
        ASSERT_TRUE(!result.has_value());
        ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
        ASSERT_EQ(QString::fromStdString(result.error().detail), QString("scripted backend reset failure"));
    }
    catch (...)
    {
        FAIL() << "standard exception crossed the configurator seam";
    }
}

TEST(SerialFacadeConfiguratorTest, aNonStandardFacadeExceptionBecomesInternalStatus)
{
    Harness harness;
    EXPECT_CALL(*harness.fake, change_port_speed(QStringLiteral("4800"))).WillOnce(ThrowNonStandardBackendFailure());

    try
    {
        const auto result = harness.configurator->apply(klineConfig());
        ASSERT_TRUE(!result.has_value());
        ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
    }
    catch (...)
    {
        FAIL() << "non-standard exception crossed the configurator seam";
    }
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
