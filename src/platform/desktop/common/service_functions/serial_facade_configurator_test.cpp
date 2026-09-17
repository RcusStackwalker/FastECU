#include "src/platform/desktop/common/service_functions/serial_facade_configurator.h"

#include <QTest>
#include <QCoreApplication>

#include <gmock/gmock.h>

#include <memory>
#include <stdexcept>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

using fastecu::ErrorKind;
using fastecu::service_functions::SerialPortActionsConfigurator;
using fastecu::service_functions::SsmTransportConfig;

namespace
{

SsmTransportConfig klineConfig()
{
    return SsmTransportConfig{
        .framing = SsmTransportConfig::Framing::Kline14230,
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
        serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
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

class SerialFacadeConfiguratorTest : public QObject
{
    Q_OBJECT

  private slots:
    void isoConfigurationClearsAStaleKlineHeaderAndUsesTheRequiredOrder()
    {
        Harness harness;
        QVERIFY(harness.serial->set_add_iso14230_header(true));

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

        QVERIFY(result.has_value());
        QCOMPARE(harness.serial->get_add_iso14230_header(), false);
    }

    void klineConfigurationPreservesLegacyOpenBaudHeaderOrder()
    {
        Harness harness;

        ::testing::InSequence sequence;
        EXPECT_CALL(*harness.fake, reset_connection()).WillOnce(::testing::Return());
        EXPECT_CALL(*harness.fake, set_is_can_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*harness.fake, set_is_iso15765_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*harness.fake, set_is_iso14230_connection(true)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*harness.fake, open_serial_port()).WillOnce(::testing::Return(QStringLiteral("fake-port")));
        EXPECT_CALL(*harness.fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*harness.fake, change_port_speed(QStringLiteral("4800")))
            .WillOnce(::testing::Return(STATUS_SUCCESS));
        EXPECT_CALL(*harness.fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*harness.fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(true));

        const auto result = harness.configurator->apply(klineConfig());

        QVERIFY(result.has_value());
    }

    void nullFacadeIsDisconnected()
    {
        SerialPortActionsConfigurator configurator{nullptr};

        const auto result = configurator.apply(SsmTransportConfig{});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    void anEmptyOpenResultIsDisconnectedEvenWithAStaleOpenFlag()
    {
        Harness harness;
        EXPECT_CALL(*harness.fake, open_serial_port()).WillOnce(::testing::Return(QString{}));

        const auto result = harness.configurator->apply(SsmTransportConfig{});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    void aPortThatIsNotOpenAfterOpenIsDisconnected()
    {
        Harness harness;
        EXPECT_CALL(*harness.fake, is_serial_port_open()).WillOnce(::testing::Return(false));
        // A port that never opened must not be driven to a new baud rate.
        EXPECT_CALL(*harness.fake, change_port_speed(::testing::_)).Times(0);

        const auto result = harness.configurator->apply(klineConfig());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    void eachBooleanSetterFailureIsInvalidConfig_data()
    {
        QTest::addColumn<int>("setter");
        QTest::newRow("set_is_iso14230_connection") << 0;
        QTest::newRow("set_is_can_connection") << 1;
        QTest::newRow("set_is_iso15765_connection") << 2;
        QTest::newRow("set_is_29_bit_id") << 3;
        QTest::newRow("set_add_iso14230_header") << 4;
        QTest::newRow("set_can_speed") << 5;
        QTest::newRow("set_iso15765_source_address") << 6;
        QTest::newRow("set_iso15765_destination_address") << 7;
        QTest::newRow("set_can_source_address") << 8;
        QTest::newRow("set_can_destination_address") << 9;
    }

    void eachBooleanSetterFailureIsInvalidConfig()
    {
        QFETCH(int, setter);
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
            EXPECT_CALL(*harness.fake, set_iso15765_destination_address(::testing::_))
                .WillOnce(::testing::Return(false));
            break;
        case 8:
            EXPECT_CALL(*harness.fake, set_can_source_address(::testing::_)).WillOnce(::testing::Return(false));
            break;
        case 9:
            EXPECT_CALL(*harness.fake, set_can_destination_address(::testing::_)).WillOnce(::testing::Return(false));
            break;
        default:
            QFAIL("unexpected configuration-setter index");
        }

        const auto result = harness.configurator->apply(SsmTransportConfig{});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
    }

    void aKlineHeaderSetterFailureIsInvalidConfig()
    {
        Harness harness;
        EXPECT_CALL(*harness.fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(false));

        const auto result = harness.configurator->apply(klineConfig());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
    }

    void aSetterExceptionBecomesInternalStatus()
    {
        Harness harness;
        EXPECT_CALL(*harness.fake, set_is_iso14230_connection(false))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend config-setter failure")));

        const auto result = harness.configurator->apply(SsmTransportConfig{});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    void anOpenExceptionBecomesInternalStatus()
    {
        Harness harness;
        EXPECT_CALL(*harness.fake, open_serial_port())
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend open failure")));

        const auto result = harness.configurator->apply(SsmTransportConfig{});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    void aRejectedBaudChangeIsInternal()
    {
        Harness harness;
        EXPECT_CALL(*harness.fake, change_port_speed(QStringLiteral("4800"))).WillOnce(::testing::Return(STATUS_ERROR));

        const auto result = harness.configurator->apply(klineConfig());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    void aPortDropDuringRejectedBaudChangeIsDisconnected()
    {
        Harness harness;
        EXPECT_CALL(*harness.fake, is_serial_port_open())
            .WillOnce(::testing::Return(true))
            .WillOnce(::testing::Return(false));
        EXPECT_CALL(*harness.fake, change_port_speed(QStringLiteral("4800"))).WillOnce(::testing::Return(STATUS_ERROR));

        const auto result = harness.configurator->apply(klineConfig());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    void aStandardFacadeExceptionBecomesInternalStatus()
    {
        Harness harness;
        EXPECT_CALL(*harness.fake, reset_connection())
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend reset failure")));

        try
        {
            const auto result = harness.configurator->apply(SsmTransportConfig{});
            QVERIFY(!result.has_value());
            QCOMPARE(result.error().kind, ErrorKind::Internal);
            QCOMPARE(QString::fromStdString(result.error().detail), QString("scripted backend reset failure"));
        }
        catch (...)
        {
            QFAIL("standard exception crossed the configurator seam");
        }
    }

    void aNonStandardFacadeExceptionBecomesInternalStatus()
    {
        Harness harness;
        EXPECT_CALL(*harness.fake, change_port_speed(QStringLiteral("4800")))
            .WillOnce(ThrowNonStandardBackendFailure());

        try
        {
            const auto result = harness.configurator->apply(klineConfig());
            QVERIFY(!result.has_value());
            QCOMPARE(result.error().kind, ErrorKind::Internal);
        }
        catch (...)
        {
            QFAIL("non-standard exception crossed the configurator seam");
        }
    }
};

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    // QCoreApplication, not QApplication: this suite instantiates no widget,
    // and a QApplication needs a platform plugin that headless CI does not
    // have. Targets that genuinely need one set QT_QPA_PLATFORM=offscreen.
    QCoreApplication application(argc, argv);
    SerialFacadeConfiguratorTest test;
    const int result = QTest::qExec(&test, argc, argv);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "serial_facade_configurator_test.moc"
