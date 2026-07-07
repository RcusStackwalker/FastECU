#include <QtTest>

#include "modules/checksum/checksum_ecu_subaru_denso_sh7xxx.h"
#include "test_checksum_results.h"

namespace {

void appendU32(QByteArray *data, quint32 value)
{
    data->append(char((value >> 24) & 0xff));
    data->append(char((value >> 16) & 0xff));
    data->append(char((value >> 8) & 0xff));
    data->append(char(value & 0xff));
}

QByteArray densoRomWithChecksumTable(quint32 storedChecksum)
{
    QByteArray rom;
    rom.append(QByteArray(4, char(0x00)));
    appendU32(&rom, 0x00000001);
    rom.append(QByteArray(8, char(0x00)));
    appendU32(&rom, 0x00000004);
    appendU32(&rom, 0x00000008);
    appendU32(&rom, storedChecksum);
    return rom;
}

} // namespace

class TestChecksumResults : public QObject
{
    Q_OBJECT

private slots:
    void densoSh7xxx_returnsUnchangedForMatchingChecksum()
    {
        const QByteArray rom = densoRomWithChecksumTable(0x5aa5a559);

        const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(rom, 16, 12);

        QCOMPARE(result.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(result.romData, rom);
        QVERIFY(result.ok());
        QVERIFY(!result.changed());
    }

    void densoSh7xxx_returnsCorrectedDataForMismatchedChecksum()
    {
        const QByteArray rom = densoRomWithChecksumTable(0x00000000);

        const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(rom, 16, 12);

        QCOMPARE(result.status, ChecksumResult::Status::Corrected);
        QCOMPARE(result.romData.mid(24, 4), QByteArray::fromHex("5aa5a559"));
        QVERIFY(result.ok());
        QVERIFY(result.changed());
    }

    void densoSh7xxx_returnsDisabledWithoutClearingRomData()
    {
        QByteArray rom(16, char(0x00));
        appendU32(&rom, 0x00000000);
        appendU32(&rom, 0x00000000);
        appendU32(&rom, 0x5aa5a55a);

        const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(rom, 16, 12);

        QCOMPARE(result.status, ChecksumResult::Status::Disabled);
        QCOMPARE(result.romData, rom);
        QVERIFY(result.ok());
    }

    void densoSh7xxx_rejectsChecksumAreaOutsideRom()
    {
        const QByteArray rom(16, char(0x00));

        const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(rom, 16, 12);

        QCOMPARE(result.status, ChecksumResult::Status::InvalidSize);
        QCOMPARE(result.romData, rom);
        QVERIFY(!result.ok());
    }

    void densoSh7xxx_rejectsNonTableAlignedChecksumArea()
    {
        const QByteArray rom(16, char(0x00));

        const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(rom, 0, 10);

        QCOMPARE(result.status, ChecksumResult::Status::ParseError);
        QCOMPARE(result.romData, rom);
        QVERIFY(!result.ok());
    }
};

int run_test_checksum_results(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestChecksumResults t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_checksum_results.moc"
