#include <QtTest>

#include "src/algorithms/diagnostics/qt_dtc_parser.h"
#include "src/algorithms/diagnostics/qt_nrc_parser.h"
#include "test_diagnostic_parsers.h"

class TestDiagnosticParsers : public QObject
{
    Q_OBJECT

  private slots:
    void nrc_decodesKnownNegativeResponse()
    {
        const QHash<int, QString> codes = {{0x31, "Request out of range"}};

        QCOMPARE(NrcParser::parse(QByteArray::fromHex("7f2231"), codes),
                 QString("Request out of range"));
    }

    void nrc_rejectsMalformedResponse()
    {
        QCOMPARE(NrcParser::parse(QByteArray::fromHex("6222"), {}), QString("Not a valid answer"));
        QCOMPARE(NrcParser::parse(QByteArray::fromHex("7f22ff"), {}), QString("Unknown error code"));
    }

    void dtc_decodesKnownCategoryMap()
    {
        const QHash<int, QString> cCodes = {{0x4001, "C0001 - Test chassis code"}};

        QCOMPARE(DtcParser::parse(0x4001, {}, cCodes, {}, {}),
                 QString("C0001 - Test chassis code"));
    }

    void dtc_usesCategoryPrefixForUnknownCodes()
    {
        QCOMPARE(DtcParser::parse(0x0001, {}, {}, {}, {}), QString("P0001 - Unknown error code"));
        QCOMPARE(DtcParser::parse(0x4001, {}, {}, {}, {}), QString("C0001 - Unknown error code"));
        QCOMPARE(DtcParser::parse(0x8001, {}, {}, {}, {}), QString("B0001 - Unknown error code"));
        QCOMPARE(DtcParser::parse(0xc001, {}, {}, {}, {}), QString("U0001 - Unknown error code"));
    }
};

int run_test_diagnostic_parsers(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestDiagnosticParsers t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_diagnostic_parsers.moc"
