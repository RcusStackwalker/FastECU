#include <QtTest>

#include "src/platform/desktop/common/flash/legacy/legacy_flash_utils.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

class TestLegacyFlashUtils : public QObject
{
    Q_OBJECT
  private slots:
    void configureIso15765Can_setsSharedCanTransportState()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new FakeBackend(); return fake; });

        FlashUtils::configureIso15765Can(&serial, "250000", 0x7E1, 0x7E9, true);

        QCOMPARE(serial.get_is_iso14230_connection(), false);
        QCOMPARE(serial.get_add_iso14230_header(), false);
        QCOMPARE(serial.get_is_can_connection(), false);
        QCOMPARE(serial.get_is_iso15765_connection(), true);
        QCOMPARE(serial.get_is_29_bit_id(), true);
        QCOMPARE(serial.get_can_speed(), QString("250000"));
        QCOMPARE(serial.get_iso15765_source_address(), quint32(0x7E1));
        QCOMPARE(serial.get_iso15765_destination_address(), quint32(0x7E9));
    }

    void configureIso15765Can_defaultsTo11BitCanIds()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new FakeBackend(); return fake; });

        FlashUtils::configureIso15765Can(&serial, "500000", 0x7E0, 0x7E8);

        QCOMPARE(serial.get_is_29_bit_id(), false);
        QCOMPARE(serial.get_can_speed(), QString("500000"));
        QCOMPARE(serial.get_iso15765_source_address(), quint32(0x7E0));
        QCOMPARE(serial.get_iso15765_destination_address(), quint32(0x7E8));
    }

    void findFlashDeviceIndex_forwardsToPortableLookup()
    {
        // The shim exists only to keep 30 legacy call sites on QString; it
        // must agree with the portable lookup exactly, including the
        // not-found sentinel.
        QCOMPARE(FlashUtils::findFlashDeviceIndex("M32R_384KB_1block"),
                 fastecu::flash::find_flash_device_index("M32R_384KB_1block"));
        QCOMPARE(FlashUtils::findFlashDeviceIndex("UNKNOWN_MCU"), -1);
        QVERIFY(FlashUtils::findFlashDeviceIndex("M32R_512KB") >= 0);
    }
};

QTEST_GUILESS_MAIN(TestLegacyFlashUtils)

#include "legacy_flash_utils_test.moc"
