#include <QtTest>

#include <array>
#include <cstdint>
#include <span>

#include "src/backend/flash/flash_utils.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
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

    void flashDeviceTable_matchesExpectedSummariesAndNamedAnomalies()
    {
        struct FlashDeviceSummary
        {
            const char *name;
            uint32_t romsize;
            unsigned numblocks;
            uint32_t firstBlockStart;
            uint32_t finalBlockEnd;
        };

        const QList<FlashDeviceSummary> expected = {
            {"M32R_128KB", 0x20000, 2, 0x0, 0x20000},
            {"M32R_256KB", 0x40000, 7, 0x0, 0x40000},
            {"M32R_384KB", 0x60000, 9, 0x0, 0x60000},
            {"M32R_512KB", 0x80000, 11, 0x0, 0x80000},
            {"M32R_512KB_1block", 0x80000, 1, 0x0, 0x80000},
            {"M32R_512KB_4blocks", 0x80000, 4, 0x0, 0x80000},
            {"M32R_384KB_1block", 0x60000, 1, 0x8000, 0x60000},
            {"MC68HC16Y5", 0x28000, 10, 0x0, 0x30000},
            {"MC68HC16Y5_TPU", 0x1000, 4, 0x60000, 0x64000},
            {"SH7051", 0x40000, 1, 0x0, 0x40000},
            {"SH7055", 0x80000, 16, 0x0, 0x80000},
            {"SH7058", 0x100000, 16, 0x0, 0x100000},
            {"SH7058_1block", 0x100000, 1, 0x0, 0x100000},
            {"SH7058d", 0x100000, 16, 0x0, 0x100000},
            {"SH7059d", 0x180000, 16, 0x0, 0x180000},
            {"SH72543d", 0x200000, 1, 0x8000, 0x1fff00},
            {"SH72531", 0x140000, 3, 0x0, 0x138000},
            {"N83M_4MB", 0x3e4000, 3, 0x08f9c000, 0x09380000},
            {"N83M_1_5MB", 0x174000, 3, 0x08f9c000, 0x09120000},
            {"SH72543R", 0x200000, 2, 0x0, 0x200000},
            {"MH8104", 0x80000, 4, 0x0, 0x80000},
            {"MH5006", 0x100000, 4, 0x0, 0x100000},
            {"MH8111", 0x180000, 4, 0x0, 0x180000},
            {"M3779x", 0x10000, 1, 0x8000, 0x17fff},
            {"M3775x", 0x10000, 1, 0x1000, 0x10fff},
        };

        for (int deviceIndex = 0; deviceIndex < expected.size(); ++deviceIndex)
        {
            const flashdev_t& actual = flashdevices[deviceIndex];
            const FlashDeviceSummary& summary = expected.at(deviceIndex);
            QCOMPARE(QString::fromLatin1(actual.name), QString::fromLatin1(summary.name));
            QCOMPARE(actual.romsize, summary.romsize);
            QCOMPARE(actual.numblocks, summary.numblocks);
            QCOMPARE(actual.fblocks[0].start, summary.firstBlockStart);
            const flashblock& finalBlock = actual.fblocks[actual.numblocks - 1];
            QCOMPARE(finalBlock.start + finalBlock.len, summary.finalBlockEnd);
            for (unsigned blockIndex = 0; blockIndex < actual.numblocks; ++blockIndex)
            {
                QVERIFY(actual.fblocks[blockIndex].len > 0);
                if (blockIndex > 0)
                    QVERIFY(actual.fblocks[blockIndex - 1].start <= actual.fblocks[blockIndex].start);
            }
        }

        const flashdev_t& sentinel = flashdevices[expected.size()];
        QCOMPARE(sentinel.name, nullptr);
        QCOMPARE(sentinel.romsize, uint32_t(0));
        QCOMPARE(sentinel.numblocks, 0u);
        QCOMPARE(sentinel.fblocks, nullptr);
        QCOMPARE(sentinel.rblocks, nullptr);
        QCOMPARE(sentinel.kblocks, nullptr);
        QCOMPARE(sentinel.eblocks, nullptr);

        const flashdev_t *sh72531 = FlashUtils::findFlashDevice("SH72531");
        const flashdev_t *mc68 = FlashUtils::findFlashDevice("MC68HC16Y5");
        const flashdev_t *n83 = FlashUtils::findFlashDevice("N83M_1_5MB");
        const flashdev_t *tpu = FlashUtils::findFlashDevice("MC68HC16Y5_TPU");
        QVERIFY(sh72531 != nullptr);
        QVERIFY(mc68 != nullptr);
        QVERIFY(n83 != nullptr);
        QVERIFY(tpu != nullptr);

        // These checks freeze the named quirks in the current table; they do
        // not declare the anomalous ranges correct.

        // SH72531 block 2 starts 0x8000 bytes before block 1 ends.
        QCOMPARE(sh72531->fblocks[1].start + sh72531->fblocks[1].len - sh72531->fblocks[2].start,
                 uint32_t(0x8000));

        // MC68HC16Y5 leaves 0x8000 between block 7 and block 8 and its final two
        // blocks extend past the declared 0x28000 ROM size.
        QCOMPARE(mc68->fblocks[8].start - (mc68->fblocks[7].start + mc68->fblocks[7].len),
                 uint32_t(0x8000));
        QCOMPARE(mc68->fblocks[9].start + mc68->fblocks[9].len, uint32_t(0x30000));

        // N83M_1_5MB block coverage extends 0x10000 past base + declared ROM size.
        QCOMPARE(n83->fblocks[2].start + n83->fblocks[2].len -
                     (n83->fblocks[0].start + n83->romsize),
                 uint32_t(0x10000));

        // MC68HC16Y5_TPU declares 0x1000 ROM bytes but exposes four contiguous
        // 0x1000 blocks; three blocks extend beyond base + romsize.
        QCOMPARE(tpu->fblocks[3].start + tpu->fblocks[3].len -
                     (tpu->fblocks[0].start + tpu->romsize),
                 uint32_t(0x3000));
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
