#include <QtTest>
#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <file_actions.h>
#include "test_ecuflash_definition_parsing.h"

class TestEcuflashDefinitionParsing : public QObject
{
    Q_OBJECT
  private slots:
    void add_list_item_defaults_new_fields_to_placeholder()
    {
        FileActions fileActions;
        FileActions::EcuCalDefStructure ecuCalDef;

        fileActions.add_ecuflash_def_list_item(&ecuCalDef);

        QCOMPARE(ecuCalDef.SubCategoryList.size(), 1);
        QCOMPARE(ecuCalDef.SubCategoryList.at(0), QString(" "));
        QCOMPARE(ecuCalDef.LevelList.at(0), QString(" "));
        QCOMPARE(ecuCalDef.UserLevelList.at(0), QString(" "));
        QCOMPARE(ecuCalDef.SwapXYList.at(0), QString(" "));
        QCOMPARE(ecuCalDef.FlipXList.at(0), QString(" "));
        QCOMPARE(ecuCalDef.FlipYList.at(0), QString(" "));
    }

    void parses_subcategory_level_userlevel_description()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString defPath = writeDefFile(dir, "TESTCAL",
            "<rom>"
            "<romid><xmlid>TESTCAL</xmlid></romid>"
            "<table name=\"Test Table\" address=\"1000\" category=\"Fuel\" "
            "subcategory=\"Primary\" level=\"2\" userlevel=\"3\" "
            "description=\"A test table\"/>"
            "</rom>");

        FileActions fileActions;
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id << "TESTCAL";
        fileActions.ConfigValuesStruct.ecuflash_def_filename << defPath;

        FileActions::EcuCalDefStructure ecuCalDef;
        fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL");

        QCOMPARE(ecuCalDef.NameList.size(), 1);
        QCOMPARE(ecuCalDef.SubCategoryList.at(0), QString("Primary"));
        QCOMPARE(ecuCalDef.LevelList.at(0), QString("2"));
        QCOMPARE(ecuCalDef.UserLevelList.at(0), QString("3"));
        QCOMPARE(ecuCalDef.DescriptionList.at(0), QString("A test table"));
    }

  private:
    static QString writeDefFile(const QTemporaryDir &dir, const QString &baseName, const QString &xml)
    {
        const QString path = dir.filePath(baseName + ".xml");
        QFile file(path);
        file.open(QIODevice::WriteOnly | QIODevice::Text);
        file.write(xml.toUtf8());
        file.close();
        return path;
    }
};

int run_test_ecuflash_definition_parsing(int argc, char **argv)
{
    // FileActions derives from QWidget (used only for its Q_OBJECT signals/config
    // state here, never shown), which requires a QApplication rather than a plain
    // QCoreApplication to construct.
    QApplication app(argc, argv);
    TestEcuflashDefinitionParsing t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_ecuflash_definition_parsing.moc"
