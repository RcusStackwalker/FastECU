#include <QtTest>

#include <array>
#include <cstdint>
#include <span>

#include "modules/flash_utils.h"
#include "serial_port_actions.h"
#include "fake_backend.h"
#include "test_flash_utils.h"

class TestFlashUtils : public QObject
{
    Q_OBJECT
  private slots:
    void findFlashDeviceIndex_returnsKnownDevice()
    {
        const int index = FlashUtils::findFlashDeviceIndex("M32R_384KB_1block");
        const flashdev_t *device = FlashUtils::findFlashDevice("M32R_384KB_1block");

        QVERIFY(index >= 0);
        QCOMPARE(QString::fromLatin1(flashdevices[index].name), QString("M32R_384KB_1block"));
        QVERIFY(device != nullptr);
        QCOMPARE(QString::fromLatin1(device->name), QString("M32R_384KB_1block"));
        QCOMPARE(device->romsize, flashdevices[index].romsize);
        QCOMPARE(device->fblocks[0].start, flashdevices[index].fblocks[0].start);
    }

    void findFlashDeviceIndex_returnsMinusOneForUnknownDevice()
    {
        QCOMPARE(FlashUtils::findFlashDeviceIndex("UNKNOWN_MCU"), -1);
        QCOMPARE(FlashUtils::findFlashDevice("UNKNOWN_MCU"), nullptr);
    }

    void buildIso15765Request_prependsBigEndianSourceAddress()
    {
        const QByteArray payload = QByteArray::fromHex("1081");

        QCOMPARE(FlashUtils::buildIso15765Request(0x7E0, payload),
                 QByteArray::fromHex("000007e01081"));
        QCOMPARE(FlashUtils::buildIso15765Request(0x18DA10F1, payload),
                 QByteArray::fromHex("18da10f11081"));
    }

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

    void cksAdd8_returnsZeroForEmptyData()
    {
        QCOMPARE(FlashUtils::cks_add8(std::span<const std::uint8_t>{}), std::uint8_t(0));
    }

    void cksAdd8_sumsBytesWithoutCarry()
    {
        const std::array<std::uint8_t, 3> bytes{0x01, 0x02, 0x03};
        QCOMPARE(FlashUtils::cks_add8(std::span<const std::uint8_t>(bytes)), std::uint8_t(6));
    }

    void cksAdd8_addsOneOnCarry()
    {
        // Plain mod-256 truncation of 0xFF + 0xFF would give 0xFE; the
        // "add 1 on carry" step this checksum is named for makes it 0xFF.
        const std::array<std::uint8_t, 2> bytes{0xFF, 0xFF};
        QCOMPARE(FlashUtils::cks_add8(std::span<const std::uint8_t>(bytes)), std::uint8_t(0xFF));
    }

    void cksAdd8_matchesReflashBlockShape()
    {
        // Matches EcuOperations::npk_raw_flashblock's real call shape: a
        // 131-byte block (3-byte address header + 128-byte payload).
        // Repeated carry corrections over 131 additions of 0x02 give 7,
        // not the naive mod-256 sum of 131*2 = 262 -> 6.
        const std::array<std::uint8_t, 131> bytes = []
        {
            std::array<std::uint8_t, 131> data{};
            data.fill(0x02);
            return data;
        }();
        QCOMPARE(FlashUtils::cks_add8(std::span<const std::uint8_t>(bytes)), std::uint8_t(7));
    }
};

int run_test_flash_utils(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestFlashUtils t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_flash_utils.moc"
