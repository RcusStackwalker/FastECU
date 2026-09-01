#include "src/platform/desktop/common/service_functions/serial_facade_configurator.h"

#include <QTest>

#include <memory>

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
                                                         fake = new FakeBackend;
                                                         return fake;
                                                     });
        serial->set_add_ssm_header(false); // start the facade's backend thread
        fake->openSerialPortResult = "fake-port";
        fake->portOpen.store(true);
        fake->logLifecycleCalls = true;
        fake->takeCallLog();
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
        harness.fake->takeCallLog();

        const auto result = harness.configurator->apply(SsmTransportConfig{});

        QVERIFY(result.has_value());
        QCOMPARE(harness.serial->get_add_iso14230_header(), false);
        QCOMPARE(
            harness.fake->takeCallLog(),
            QStringList({"reset_connection", "cfg:set_is_iso14230_connection:0", "cfg:set_is_can_connection:0",
                         "cfg:set_is_iso15765_connection:1", "cfg:set_is_29_bit_id:0", "cfg:set_add_iso14230_header:0",
                         "cfg:set_can_speed:500000", "cfg:set_iso15765_source_address:2017",
                         "cfg:set_iso15765_destination_address:2025", "cfg:set_can_source_address:2017",
                         "cfg:set_can_destination_address:2025", "open_serial_port", "is_serial_port_open"}));
    }

    void klineConfigurationPreservesLegacyOpenBaudHeaderOrder()
    {
        Harness harness;

        const auto result = harness.configurator->apply(klineConfig());

        QVERIFY(result.has_value());
        QCOMPARE(harness.fake->takeCallLog(),
                 QStringList({"reset_connection", "cfg:set_is_can_connection:0", "cfg:set_is_iso15765_connection:0",
                              "cfg:set_is_iso14230_connection:1", "open_serial_port", "is_serial_port_open",
                              "baud:begin:4800", "baud:end", "is_serial_port_open", "cfg:set_add_iso14230_header:0"}));
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
        harness.fake->openSerialPortResult.clear();
        harness.fake->portOpen.store(true);

        const auto result = harness.configurator->apply(SsmTransportConfig{});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    void aPortThatIsNotOpenAfterOpenIsDisconnected()
    {
        Harness harness;
        harness.fake->portOpen.store(false);

        const auto result = harness.configurator->apply(klineConfig());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
        QVERIFY(harness.fake->takeCallLog().filter("baud:begin").isEmpty());
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
            harness.fake->isIso14230ConnectionResult = false;
            break;
        case 1:
            harness.fake->isCanConnectionResult = false;
            break;
        case 2:
            harness.fake->isIso15765ConnectionResult = false;
            break;
        case 3:
            harness.fake->is29BitIdResult = false;
            break;
        case 4:
            harness.fake->addIso14230HeaderResult = false;
            break;
        case 5:
            harness.fake->canSpeedResult = false;
            break;
        case 6:
            harness.fake->iso15765SourceAddressResult = false;
            break;
        case 7:
            harness.fake->iso15765DestinationAddressResult = false;
            break;
        case 8:
            harness.fake->canSourceAddressResult = false;
            break;
        case 9:
            harness.fake->canDestinationAddressResult = false;
            break;
        }

        const auto result = harness.configurator->apply(SsmTransportConfig{});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
    }

    void aKlineHeaderSetterFailureIsInvalidConfig()
    {
        Harness harness;
        harness.fake->addIso14230HeaderResult = false;

        const auto result = harness.configurator->apply(klineConfig());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
    }

    void aSetterExceptionBecomesInternalStatus()
    {
        Harness harness;
        harness.fake->throwOnConfigSetter = true;

        const auto result = harness.configurator->apply(SsmTransportConfig{});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    void anOpenExceptionBecomesInternalStatus()
    {
        Harness harness;
        harness.fake->throwOnOpen = true;

        const auto result = harness.configurator->apply(SsmTransportConfig{});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    void aRejectedBaudChangeIsInternal()
    {
        Harness harness;
        harness.fake->baudChangeResult = STATUS_ERROR;

        const auto result = harness.configurator->apply(klineConfig());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    void aPortDropDuringRejectedBaudChangeIsDisconnected()
    {
        Harness harness;
        harness.fake->baudChangeResult = STATUS_ERROR;
        harness.fake->closePortAfterBaud = true;

        const auto result = harness.configurator->apply(klineConfig());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    void aStandardFacadeExceptionBecomesInternalStatus()
    {
        Harness harness;
        harness.fake->throwOnReset = true;

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
        harness.fake->throwNonStandardOnBaudChange = true;

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

QTEST_MAIN(SerialFacadeConfiguratorTest)
#include "serial_facade_configurator_test.moc"
