#include <QIcon>
#include <QTest>
#include <QTreeWidget>

#include "src/ui/desktop/calibration/session_key.h"
#include "src/ui/desktop/calibration_treewidget.h"

namespace
{

using fastecu::calibration::CalibrationSession;
using fastecu::calibration::SessionContents;
using fastecu::calibration::SessionId;

fastecu::definition::CalibrationMap map(std::string name, std::string category, std::string type, std::uint32_t x,
                                        std::uint32_t y, std::string description = {}, std::string id = {})
{
    fastecu::definition::CalibrationMap m;
    m.id = std::move(id);
    m.name = std::move(name);
    m.category = std::move(category);
    m.type = std::move(type);
    m.x_size = x;
    m.y_size = y;
    m.description = std::move(description);
    return m;
}

CalibrationSession session_with_maps()
{
    fastecu::definition::RomDefinition definition{.format = fastecu::definition::DefinitionFormat::EcuFlash};
    definition.identity.xml_id = "TREE";
    definition.maps = {
        map("Idle", "Idle", "1D", 1, 1, "Idle speed", "idle-id"), // 0
        map("Fuel", "Fuel", "2D", 4, 1),                          // 1: no description
        map("Timing", "Fuel", "3D", 4, 2),                        // 2
        map("Mode", "Switches", "Selectable", 1, 1),              // 3
        map("Hidden", "", "1D", 1, 1),                            // 4: no category -> not listed
        map("", "Fuel", "1D", 1, 1),                              // 5: no name -> not listed
        map("Tiny", "Fuel", "3D", 1, 1),                          // 6: 1x1 -> 1D icon
    };
    return CalibrationSession(
        SessionId{7},
        SessionContents{
            .source = {.display_name = "t.bin", .path = "/t.bin"},
            .rom = std::vector<std::uint8_t>(16, 0),
            .definition = fastecu::calibration::ResolvedDefinition{.id = "TREE", .definition = definition},
            .protocol = {.file_size_label = "0kb"},
        });
}

bool same_icon(const QTreeWidgetItem *item, const char *path)
{
    return item->icon(0).pixmap(16).toImage() == QIcon(path).pixmap(16).toImage();
}

} // namespace

class CalibrationTreeWidgetTest : public QObject
{
    Q_OBJECT

  private slots:
    void filesTreeCarriesNameFirstMapIdAndSessionKey()
    {
        QTreeWidget files;
        CalibrationTreeWidget builder;
        const CalibrationSession session = session_with_maps();

        builder.buildCalibrationFilesTree(session.id(), &files, session);

        QCOMPARE(files.topLevelItemCount(), 1);
        QTreeWidgetItem *item = files.topLevelItem(0);
        QCOMPARE(item->text(0), QString("t.bin"));
        QCOMPARE(item->text(1), QString("idle-id"));
        QCOMPARE(item->text(2), fastecu::ui::session_key_text(SessionId{7}));
        QCOMPARE(item->checkState(0), Qt::Checked);
        QVERIFY(item->isSelected());
    }

    void dataTreeMatchesLegacyRules()
    {
        QTreeWidget data;
        CalibrationTreeWidget builder;
        const CalibrationSession session = session_with_maps();
        fastecu::ui::CalibrationViewState view;
        view.open_maps = {2};
        view.expanded_categories = {"Fuel"};
        view.rom_info_expanded = true;

        builder.buildCalibrationDataTree(&data, session, view);

        QCOMPARE(data.topLevelItemCount(), 4);
        QTreeWidgetItem *rom_info = data.topLevelItem(0);
        QCOMPARE(rom_info->text(0), QString("ROM Info"));
        QVERIFY(rom_info->isExpanded());
        QCOMPARE(rom_info->childCount(), 16);
        QCOMPARE(rom_info->child(0)->text(0), QString("XML ID: TREE"));

        QCOMPARE(data.topLevelItem(1)->text(0), QString("Idle"));
        QVERIFY(!data.topLevelItem(1)->isExpanded());
        QTreeWidgetItem *fuel = data.topLevelItem(2);
        QCOMPARE(fuel->text(0), QString("Fuel"));
        QVERIFY(fuel->isExpanded());
        QCOMPARE(data.topLevelItem(3)->text(0), QString("Switches"));

        QCOMPARE(fuel->childCount(), 3); // Fuel, Timing, Tiny; the nameless map is skipped
        QCOMPARE(fuel->child(0)->text(0), QString("Fuel"));
        QCOMPARE(fuel->child(0)->text(1), QString("1"));
        QCOMPARE(fuel->child(0)->toolTip(0), QString("Fuel ")); // legacy_value(" ") description
        QCOMPARE(fuel->child(0)->checkState(0), Qt::Unchecked);
        QVERIFY(same_icon(fuel->child(0), ":/icons/2D-64.png"));
        QCOMPARE(fuel->child(1)->text(1), QString("2"));
        QCOMPARE(fuel->child(1)->checkState(0), Qt::Checked);
        QVERIFY(same_icon(fuel->child(1), ":/icons/3D-64.png"));
        QCOMPARE(fuel->child(2)->text(1), QString("6"));
        QVERIFY(same_icon(fuel->child(2), ":/icons/1D-64.png"));

        QTreeWidgetItem *idle = data.topLevelItem(1)->child(0);
        QCOMPARE(idle->toolTip(0), QString("IdleIdle speed"));
        QVERIFY(same_icon(idle, ":/icons/1D-64.png"));
        QVERIFY(same_icon(data.topLevelItem(3)->child(0), ":/icons/1D-64.png")); // Selectable
    }

    void definitionlessRomShowsOnlyRomInfo()
    {
        QTreeWidget data;
        CalibrationTreeWidget builder;
        const CalibrationSession session(SessionId{1}, SessionContents{.rom = std::vector<std::uint8_t>(4, 0)});

        builder.buildCalibrationDataTree(&data, session, {});

        QCOMPARE(data.topLevelItemCount(), 1);
        QCOMPARE(data.topLevelItem(0)->text(0), QString("ROM Info"));
        QVERIFY(!data.topLevelItem(0)->isExpanded());
    }
};

QTEST_MAIN(CalibrationTreeWidgetTest)
#include "calibration_treewidget_test.moc"
