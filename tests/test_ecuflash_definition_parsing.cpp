#include <QtTest>
#include <QApplication>
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
