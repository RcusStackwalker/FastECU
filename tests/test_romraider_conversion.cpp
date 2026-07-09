#include <QtTest>
#include <cstdio>
#include <QApplication>
#include <QDir>
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
    fprintf(stderr, "[diag] QT_QPA_PLATFORM='%s'\n", qEnvironmentVariable("QT_QPA_PLATFORM").toUtf8().constData());
    fflush(stderr);
    {
        const QString pluginPath = qEnvironmentVariable("QT_QPA_PLATFORM_PLUGIN_PATH");
        const QString genericPluginPath = qEnvironmentVariable("QT_PLUGIN_PATH");
        fprintf(stderr, "[diag] QT_QPA_PLATFORM_PLUGIN_PATH='%s'\n", pluginPath.toUtf8().constData());
        fprintf(stderr, "[diag] QT_PLUGIN_PATH='%s'\n", genericPluginPath.toUtf8().constData());
        QDir pluginDir(pluginPath);
        fprintf(stderr, "[diag] plugin dir exists=%d absolutePath='%s' entries='%s'\n",
                pluginDir.exists() ? 1 : 0,
                pluginDir.absolutePath().toUtf8().constData(),
                pluginDir.entryList(QDir::Files).join(", ").toUtf8().constData());
        fflush(stderr);
    }
    fprintf(stderr, "[diag] before QApplication construction\n");
    fflush(stderr);
    QApplication app(argc, argv);
    fprintf(stderr, "[diag] after QApplication construction\n");
    fflush(stderr);
    TestRomraiderConversion t;
    fprintf(stderr, "[diag] before QTest::qExec\n");
    fflush(stderr);
    return QTest::qExec(&t, argc, argv);
}
#include "test_romraider_conversion.moc"
