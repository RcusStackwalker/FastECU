#include <QtTest>
#include <QApplication>
#include <file_actions.h>
#include "logging/romraider_conversion.h"
#include "test_romraider_conversion.h"

class TestRomraiderConversion : public QObject
{
    Q_OBJECT
  private slots:
    void applies_scaling_expression_and_format()
    {
        FileActions fileActions;
        FileActions::LogValuesStructure lv;
        lv.log_value_units << "Engine Speed,rpm,x*0.25,0.00";

        QString result = convertRomRaiderValue(&fileActions, &lv, 0, "400");
        QCOMPARE(result, QString("100.00"));
    }

    void applies_integer_format_with_no_decimals()
    {
        FileActions fileActions;
        FileActions::LogValuesStructure lv;
        lv.log_value_units << "Coolant Temperature,C,x,0";

        QString result = convertRomRaiderValue(&fileActions, &lv, 0, "42");
        QCOMPARE(result, QString("42"));
    }
};

int run_test_romraider_conversion(int argc, char **argv)
{
    // FileActions derives from QWidget (used only for its Q_OBJECT signals/config
    // state here, never shown), which requires a QApplication rather than a plain
    // QCoreApplication to construct.
    QApplication app(argc, argv);
    TestRomraiderConversion t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_romraider_conversion.moc"
