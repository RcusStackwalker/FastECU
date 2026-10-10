#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <algorithm>
#include <array>
#include <string>
#include <tuple>
#include <vector>

#include <QCheckBox>
#include <QLabel>
#include <QComboBox>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <QKeyEvent>
#include <QMdiSubWindow>
#include <QTableWidget>
#include <gtest/gtest.h>
#include "src/backend/ports/testing/result_matchers.h"

#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/testing/fake_definition_catalogs.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/ui/desktop/calibration/map_edit_adapter.h"
#include "src/ui/desktop/widgets/calibration_maps.h"

namespace
{
using fastecu::calibration::SessionId;
using fastecu::definition::DefinitionFormat;

struct MapFixture
{
    fastecu::config::testing::ConfigSessionFixture cfg;
    fastecu::InMemoryAtomicFileWriter writer;
    fastecu::definition::DefinitionService definitions{cfg.file_system, cfg.file_repository, writer};
    fastecu::calibration::testing::FakeDefinitionCatalogs catalogs;
    fastecu::calibration::RomOpenUseCase opener{catalogs,        definitions, cfg.file_repository,
                                                cfg.file_system, cfg.events,  cfg.session};
    fastecu::calibration::CalibrationWorkspace workspace{opener};

    fastecu::Result<SessionId> open(std::string_view table, bool constant = false)
    {
        const auto initialized = cfg.Initialize();
        if (!initialized.has_value())
        {
            return std::unexpected(initialized.error());
        }
        auto& settings = cfg.session.Settings();
        settings.primary_definition_base = "ecuflash";
        settings.use_ecuflash_definitions = "enabled";
        settings.ecuflash_definition_files_directory = "/defs/";
        const std::string xml = std::string(R"xml(<rom>
          <romid><xmlid>MAPS</xmlid><internalidaddress>0</internalidaddress>
            <internalidstring>MAPS</internalidstring></romid>
          <scaling name="Raw" toexpr="x" frexpr="x" format="%.1f" storagetype="uint8" endian="big"/>
          <scaling name="Modes" storagetype="bloblist"><data name="Off" value="0000"/>
            <data name="On" value="0001"/></scaling>
        )xml") + std::string(table) +
                                "</rom>";
        cfg.Put("/defs/maps.xml", xml);
        cfg.file_system.files["/defs/maps.xml"] = {};
        catalogs.entries[DefinitionFormat::kEcuFlash] = {
            {.format = DefinitionFormat::kEcuFlash,
             .definition_id = "MAPS",
             .internal_id = "MAPS",
             .internal_id_address = fastecu::memory::DefinitionAddress{0},
             .internal_id_encoding = fastecu::definition::IdEncoding::kAscii,
             .source = "/defs/maps.xml"}};
        std::vector<std::uint8_t> bytes(128);
        const std::string identity = "MAPS";
        std::copy(identity.begin(), identity.end(), bytes.begin());
        for (int i = 0; i < 4; ++i)
        {
            bytes[32 + i] = static_cast<std::uint8_t>(constant ? 10 : 10 + 10 * i);
        }
        bytes[64] = 1;
        bytes[65] = 2;
        bytes[80] = 3;
        bytes[81] = 4;
        bytes[97] = 1;
        cfg.file_repository.files["/cal/maps.bin"] = std::move(bytes);
        const auto opened = workspace.OpenFile("/cal/maps.bin");
        if (!opened.has_value())
        {
            return std::unexpected(opened.error());
        }
        if (workspace.Find(opened->id)->Definition() == nullptr)
        {
            return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "synthetic map definition did not resolve");
        }
        return opened->id;
    }
};

constexpr auto kXAxis = R"(<table type="X Axis" name="Speed" address="40" elements="2" scaling="Raw"/>)";
constexpr auto kYAxis = R"(<table type="Y Axis" name="Load" address="50" elements="2" scaling="Raw"/>)";

std::string numericTable(std::string_view type, int x, int y, std::string_view axes = {})
{
    return "<table name=\"Timing\" type=\"" + std::string(type) + "\" address=\"20\" scaling=\"Raw\" sizex=\"" +
           std::to_string(x) + "\" sizey=\"" + std::to_string(y) + "\">" + std::string(axes) + "</table>";
}

void replaceDefinition(fastecu::calibration::CalibrationSession& session,
                       fastecu::calibration::ResolvedDefinition definition)
{
    const auto rom = session.Rom();
    fastecu::calibration::SessionContents contents{.source = session.Source(),
                                                   .rom = std::vector<std::uint8_t>(rom.begin(), rom.end()),
                                                   .definition = std::move(definition),
                                                   .protocol = session.Protocol()};
    session = fastecu::calibration::CalibrationSession(session.Id(), std::move(contents));
}

QTableWidget *tableOf(CalibrationMaps& map)
{
    return map.findChild<QTableWidget *>();
}
} // namespace

struct LayoutsAndRefreshCase
{
    std::string name;
    QString type;
    int x;
    int y;
    int rows;
    int cols;
    int body_row;
    int body_col;
};
class LayoutsAndRefreshParameters : public ::testing::Test, public ::testing::WithParamInterface<LayoutsAndRefreshCase>
{
};

INSTANTIATE_TEST_SUITE_P(Rows, LayoutsAndRefreshParameters,
                         ::testing::Values(LayoutsAndRefreshCase{"1D", "1D", 1, 1, 1, 1, 0, 0},
                                           LayoutsAndRefreshCase{"X_2D", "2D", 2, 1, 2, 2, 1, 0},
                                           LayoutsAndRefreshCase{"Y_2D", "2D", 1, 2, 2, 2, 0, 1},
                                           LayoutsAndRefreshCase{"3D", "3D", 2, 2, 3, 3, 1, 1}),
                         [](const ::testing::TestParamInfo<LayoutsAndRefreshCase>& info) { return info.param.name; });

TEST_P(LayoutsAndRefreshParameters, layoutsAndRefresh)
{
    const QString type = GetParam().type;
    const int x = GetParam().x;
    const int y = GetParam().y;
    const int rows = GetParam().rows;
    const int cols = GetParam().cols;
    const int bodyRow = GetParam().body_row;
    const int bodyCol = GetParam().body_col;
    MapFixture fixture;
    const auto id = fixture.open(
        numericTable(type.toStdString(), x, y,
                     (x > 1 ? std::string(kXAxis) : std::string{}) + (y > 1 ? std::string(kYAxis) : std::string{})));
    ASSERT_TRUE(id.has_value());
    if (type == "2D" && y > 1)
    {
        // EcuFlash normalizes a 2D Y axis into its X dimension. Supply
        // the legacy vertical resolved geometry the UI still supports.
        auto *session = fixture.workspace.Find(*id);
        auto definition = *session->Definition();
        auto& typedMap = definition.definition.maps[0];
        typedMap.x_size = 1;
        typedMap.y_size = 2;
        typedMap.y_axis = typedMap.x_axis;
        typedMap.x_axis = {};
        replaceDefinition(*session, std::move(definition));
    }
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = tableOf(map);
    ASSERT_TRUE(table != nullptr);
    ASSERT_EQ(table->rowCount(), rows);
    ASSERT_EQ(table->columnCount(), cols);
    ASSERT_TRUE(table->item(bodyRow, bodyCol) != nullptr);
    ASSERT_EQ(table->item(bodyRow, bodyCol)->text(), "10.0");
    ASSERT_EQ(table->item(bodyRow + (y > 1 ? y - 1 : 0), bodyCol + (x > 1 ? x - 1 : 0))->text(),
              QString::number(10 * x * y, 'f', 1));
    if (x > 1)
    {
        ASSERT_EQ(table->item(0, bodyCol)->text(), "1.0");
        ASSERT_EQ(table->item(0, bodyCol + 1)->text(), "2.0");
    }
    if (y > 1)
    {
        ASSERT_EQ(table->item(bodyRow, 0)->text(), "3.0");
        ASSERT_EQ(table->item(bodyRow + 1, 0)->text(), "4.0");
    }
    fastecu::testing::SignalRecorder changed(table, &QTableWidget::cellChanged);
    const std::array<std::uint8_t, 1> edit{55};
    ASSERT_TRUE(fixture.workspace.Find(*id)->WriteBytes(32, edit).has_value());
    map.refresh();
    ASSERT_EQ(table->item(bodyRow, bodyCol)->text(), "55.0");
    ASSERT_EQ(changed.Count(), 0U);
    ASSERT_EQ(table->rowCount(), rows);
    ASSERT_EQ(table->columnCount(), cols);
}

struct StaticAxisLabelsCase
{
    std::string name;
    QString axis_type;
};
class StaticAxisLabelsParameters : public ::testing::Test, public ::testing::WithParamInterface<StaticAxisLabelsCase>
{
};

INSTANTIATE_TEST_SUITE_P(Rows, StaticAxisLabelsParameters,
                         ::testing::Values(StaticAxisLabelsCase{"static_X", "Static X Axis"},
                                           StaticAxisLabelsCase{"static_Y", "Static Y Axis"}),
                         [](const ::testing::TestParamInfo<StaticAxisLabelsCase>& info) { return info.param.name; });

TEST_P(StaticAxisLabelsParameters, staticAxisLabels)
{
    const QString axisType = GetParam().axis_type;
    MapFixture fixture;
    const std::string axis = "<table type=\"" + std::string("Static X Axis") +
                             "\" name=\"Labels\" elements=\"2\"><data>Low</data><data>High</data></table>";
    const auto id = fixture.open(numericTable("2D", 2, 1, axis));
    ASSERT_TRUE(id.has_value());
    if (axisType == "Static Y Axis")
    {
        // The retained UI accepts this legacy tag; the typed resolver
        // currently emits only Static X Axis, so supply its equivalent
        // resolved shape directly after opening the normal fixture.
        auto *session = fixture.workspace.Find(*id);
        auto definition = *session->Definition();
        definition.definition.maps[0].x_axis.type = "Static Y Axis";
        replaceDefinition(*session, std::move(definition));
    }
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = tableOf(map);
    ASSERT_EQ(table->rowCount(), 2);
    ASSERT_EQ(table->columnCount(), 2);
    ASSERT_EQ(table->item(0, 0)->text(), "Low");
    ASSERT_EQ(table->item(0, 1)->text(), "High");
    const std::array<std::uint8_t, 1> edit{77};
    ASSERT_TRUE(fixture.workspace.Find(*id)->WriteBytes(32, edit).has_value());
    map.refresh();
    ASSERT_EQ(table->item(0, 0)->text(), "Low");
    ASSERT_EQ(table->item(1, 0)->text(), "77.0");
}

TEST(CalibrationMapsTest, absentAxisUsesSequentialFallback)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("2D", 2, 1));
    ASSERT_TRUE(id.has_value());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = tableOf(map);
    ASSERT_EQ(table->item(0, 0)->text(), "0");
    ASSERT_EQ(table->item(0, 1)->text(), "1");
    map.refresh();
    ASSERT_EQ(table->item(0, 0)->text(), "0");
    ASSERT_EQ(table->item(0, 1)->text(), "1");
}

TEST(CalibrationMapsTest, selectableReflectsBlobBytesWithoutEmittingEditSignal)
{
    MapFixture fixture;
    const auto id = fixture.open(R"(<table name="Mode" type="Selectable" address="60" scaling="Modes"/>)");
    ASSERT_TRUE(id.has_value());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = tableOf(map);
    auto *combo = qobject_cast<QComboBox *>(table->cellWidget(0, 0));
    ASSERT_TRUE(combo != nullptr);
    ASSERT_EQ(table->rowCount(), 1);
    ASSERT_EQ(table->columnCount(), 1);
    ASSERT_EQ(combo->count(), 2);
    ASSERT_EQ(combo->currentText(), "On");
    fastecu::testing::SignalRecorder edits(&map, &CalibrationMaps::selectableComboboxItemChanged);
    fastecu::testing::SignalRecorder changes(combo, &QComboBox::currentTextChanged);
    const std::array<std::uint8_t, 2> off{0, 0};
    ASSERT_TRUE(fixture.workspace.Find(*id)->WriteBytes(96, off).has_value());
    map.refresh();
    ASSERT_EQ(combo->currentText(), "Off");
    ASSERT_EQ(edits.Count(), 0U);
    ASSERT_EQ(changes.Count(), 0U);
    ASSERT_EQ(table->cellWidget(0, 0), combo);
}

TEST(CalibrationMapsTest, retainedMultiSelectableGeometryKeepsLegacyNumericCell)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("1D", 1, 1));
    ASSERT_TRUE(id.has_value());
    auto *session = fixture.workspace.Find(*id);
    auto definition = *session->Definition();
    auto& typedMap = definition.definition.maps[0];
    typedMap.type = "MultiSelectable";
    typedMap.y_axis.type = "Y Axis";
    typedMap.y_axis.name = "Labels";
    typedMap.y_axis.units = "Label";
    replaceDefinition(*session, std::move(definition));
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = tableOf(map);
    ASSERT_EQ(table->rowCount(), 1);
    ASSERT_EQ(table->columnCount(), 1);
    ASSERT_TRUE(table->item(0, 0) != nullptr);
    ASSERT_EQ(table->item(0, 0)->text(), "10.0");
    const std::array<std::uint8_t, 1> edit{55};
    ASSERT_TRUE(session->WriteBytes(32, edit).has_value());
    fastecu::testing::SignalRecorder changed(table, &QTableWidget::cellChanged);
    map.refresh();
    ASSERT_EQ(table->item(0, 0)->text(), "55.0");
    ASSERT_EQ(changed.Count(), 0U);
}

TEST(CalibrationMapsTest, retainedSwitchRefreshKeepsUncheckedControlWithoutEmittingEdits)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("1D", 1, 1));
    ASSERT_TRUE(id.has_value());
    auto *session = fixture.workspace.Find(*id);
    auto definition = *session->Definition();
    definition.definition.maps[0].type = "Switch";
    replaceDefinition(*session, std::move(definition));
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = tableOf(map);
    ASSERT_EQ(table->rowCount(), 1);
    ASSERT_EQ(table->columnCount(), 1);
    auto *checkbox = qobject_cast<QCheckBox *>(table->cellWidget(0, 0));
    ASSERT_TRUE(checkbox != nullptr);
    ASSERT_TRUE(!checkbox->isChecked());
    fastecu::testing::SignalRecorder edits(&map, &CalibrationMaps::checkboxStateChanged);
    checkbox->setChecked(true);
    ASSERT_EQ(edits.Count(), 1U);
    const auto editsBeforeRefresh = edits.Count();
    map.refresh();
    ASSERT_EQ(table->cellWidget(0, 0), checkbox);
    ASSERT_TRUE(!checkbox->isChecked());
    ASSERT_EQ(edits.Count(), editsBeforeRefresh);
    ASSERT_TRUE(!session->Dirty());
}

TEST(CalibrationMapsTest, switchCheckboxEmitsQtCheckStateValues)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("1D", 1, 1));
    ASSERT_TRUE(id.has_value());
    auto *session = fixture.workspace.Find(*id);
    auto definition = *session->Definition();
    definition.definition.maps[0].type = "Switch";
    replaceDefinition(*session, std::move(definition));
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *checkbox = qobject_cast<QCheckBox *>(tableOf(map)->cellWidget(0, 0));
    ASSERT_TRUE(checkbox != nullptr);
    fastecu::testing::SignalRecorder edits(&map, &CalibrationMaps::checkboxStateChanged);
    checkbox->setChecked(true);
    checkbox->setChecked(false);
    ASSERT_EQ(edits.Count(), 2U);
    EXPECT_EQ(std::get<0>(edits.Snapshot().at(0)), static_cast<int>(Qt::Checked));
    EXPECT_EQ(std::get<0>(edits.Snapshot().at(1)), static_cast<int>(Qt::Unchecked));
}

TEST(CalibrationMapsTest, colorsKeepOpeningBoundsDuringRefreshAndReopenUsesCurrentValues)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("2D", 2, 1, kXAxis));
    ASSERT_TRUE(id.has_value());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = tableOf(map);
    const auto minimumColor = table->item(1, 0)->background().color();
    const auto maximumColor = table->item(1, 1)->background().color();
    ASSERT_TRUE(minimumColor != maximumColor);
    const std::array<std::uint8_t, 1> edit{30};
    ASSERT_TRUE(fixture.workspace.Find(*id)->WriteBytes(32, edit).has_value());
    map.refresh();
    ASSERT_EQ(table->item(1, 0)->background().color(), maximumColor);
    ASSERT_EQ(table->item(1, 1)->background().color(), maximumColor);
    CalibrationMaps reopened(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *reopenedTable = tableOf(reopened);
    ASSERT_EQ(reopenedTable->item(1, 0)->background().color(), maximumColor);
    ASSERT_EQ(reopenedTable->item(1, 1)->background().color(), minimumColor);
    // Reopening must not alter the already open window's color bounds.
    map.refresh();
    ASSERT_EQ(table->item(1, 1)->background().color(), maximumColor);
}

TEST(CalibrationMapsTest, constantMapHasFiniteStableColors)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("2D", 2, 1, kXAxis), true);
    ASSERT_TRUE(id.has_value());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = tableOf(map);
    const auto color = table->item(1, 0)->background().color();
    ASSERT_TRUE(color.isValid());
    ASSERT_EQ(table->item(1, 1)->background().color(), color);
    const std::array<std::uint8_t, 1> edit{30};
    ASSERT_TRUE(fixture.workspace.Find(*id)->WriteBytes(32, edit).has_value());
    map.refresh();
    ASSERT_EQ(table->item(1, 0)->background().color(), color);
    CalibrationMaps reopened(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    ASSERT_EQ(tableOf(reopened)->item(1, 1)->background().color(), color);
}

TEST(CalibrationMapsTest, closedSessionRefreshIsInertAfterAnotherSessionOpens)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("1D", 1, 1));
    ASSERT_TRUE(id.has_value());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = tableOf(map);
    fastecu::testing::SignalRecorder changed(table, &QTableWidget::cellChanged);
    ASSERT_TRUE(fixture.workspace.Close(*id).has_value());
    const auto replacement = fixture.workspace.OpenFile("/cal/maps.bin");
    ASSERT_TRUE(replacement.has_value());
    ASSERT_TRUE(replacement->id != *id);
    const std::array<std::uint8_t, 1> edit{99};
    ASSERT_TRUE(fixture.workspace.Find(replacement->id)->WriteBytes(32, edit).has_value());
    map.refresh();
    ASSERT_EQ(table->item(0, 0)->text(), "10.0");
    ASSERT_EQ(changed.Count(), 0U);
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}

TEST(CalibrationMaps, InvalidNumericCellShowsNanAndDiagnostic)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("1D", 1, 1));
    ASSERT_THAT(id, fastecu::testing::IsOk());
    auto *session = fixture.workspace.Find(*id);
    auto definition = *session->Definition();
    for (auto& scaling : definition.definition.scalings)
    {
        if (scaling.name == definition.definition.maps[0].scaling_name)
        {
            scaling.from_byte = "1/(x-10)";
        }
    }
    replaceDefinition(*session, std::move(definition));
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *item = tableOf(map)->item(0, 0);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->text(), "NaN");
    EXPECT_FALSE(item->toolTip().isEmpty());
    EXPECT_EQ(item->background().color(), QColor(Qt::white));
}

TEST(CalibrationMaps, StructuralRefreshFailureClearsStaleValuesAndRecovers)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("1D", 1, 1));
    ASSERT_THAT(id, fastecu::testing::IsOk());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = tableOf(map);
    ASSERT_NE(table->item(0, 0), nullptr);
    auto *session = fixture.workspace.Find(*id);
    const auto validDefinition = *session->Definition();
    auto broken = validDefinition;
    broken.definition.maps[0].address = fastecu::memory::DefinitionAddress{1000};
    replaceDefinition(*session, std::move(broken));
    map.refresh();
    EXPECT_FALSE(table->isEnabled());
    EXPECT_EQ(table->rowCount(), 0);
    const auto *error = map.findChild<QLabel *>("mapDecodeError");
    ASSERT_NE(error, nullptr);
    EXPECT_FALSE(error->text().isEmpty());
    EXPECT_FALSE(error->isHidden());
    replaceDefinition(*session, validDefinition);
    map.refresh();
    EXPECT_TRUE(table->isEnabled());
    ASSERT_NE(table->item(0, 0), nullptr);
    EXPECT_EQ(table->item(0, 0)->text(), "10.0");
    EXPECT_TRUE(error->isHidden());
}

// MainWindow names the MDI sub-window once, when the map opens. A map that
// fails its first decode is renamed later, so the edit lookup must not depend
// on the sub-window and table names matching.
TEST(CalibrationMaps, EditTargetResolvesThroughMdiWindowAfterOpeningFailureRecovers)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("1D", 1, 1));
    ASSERT_THAT(id, fastecu::testing::IsOk());
    auto *session = fixture.workspace.Find(*id);
    const auto validDefinition = *session->Definition();
    auto broken = validDefinition;
    broken.definition.maps[0].address = fastecu::memory::DefinitionAddress{1000};
    replaceDefinition(*session, std::move(broken));
    auto *map = new CalibrationMaps(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    QMdiSubWindow window;
    window.setWidget(map);
    window.setObjectName(map->objectName());
    replaceDefinition(*session, validDefinition);
    map->refresh();
    ASSERT_NE(window.objectName(), map->objectName());
    auto *table = tableOf(*map);
    table->setRangeSelected(QTableWidgetSelectionRange(table->rowCount() - 1, table->columnCount() - 1,
                                                       table->rowCount() - 1, table->columnCount() - 1),
                            true);

    EXPECT_TRUE(fastecu::ui::selectedNumericTarget(&window, *session, 0).has_value());
}

TEST(CalibrationMaps, SelectAllSelectsTheBodyAndLeavesTheAxesOut)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("3D", 2, 2, std::string(kXAxis) + std::string(kYAxis)));
    ASSERT_THAT(id, fastecu::testing::IsOk());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = tableOf(map);
    table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 0, 0), true);

    QKeyEvent selectAll(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    QApplication::sendEvent(table, &selectAll);

    const auto ranges = table->selectedRanges();
    ASSERT_EQ(ranges.size(), 1);
    EXPECT_EQ(ranges.first().topRow(), 1);
    EXPECT_EQ(ranges.first().leftColumn(), 1);
    EXPECT_EQ(ranges.first().bottomRow(), table->rowCount() - 1);
    EXPECT_EQ(ranges.first().rightColumn(), table->columnCount() - 1);
}

TEST(CalibrationMaps, StructuralFailureAtOpeningShowsError)
{
    MapFixture fixture;
    const auto id = fixture.open(numericTable("1D", 1, 1));
    ASSERT_THAT(id, fastecu::testing::IsOk());
    auto *session = fixture.workspace.Find(*id);
    auto broken = *session->Definition();
    broken.definition.maps[0].address = fastecu::memory::DefinitionAddress{1000};
    replaceDefinition(*session, std::move(broken));
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    EXPECT_FALSE(tableOf(map)->isEnabled());
    const auto *error = map.findChild<QLabel *>("mapDecodeError");
    ASSERT_NE(error, nullptr);
    EXPECT_FALSE(error->text().isEmpty());
}

TEST(CalibrationMaps, StaticLabelContainingCommaRemainsOneLabel)
{
    MapFixture fixture;
    const std::string axis =
        R"(<table type="Static X Axis" name="Labels" elements="2"><data>Low, load</data><data>High</data></table>)";
    const auto id = fixture.open(numericTable("2D", 2, 1, axis));
    ASSERT_THAT(id, fastecu::testing::IsOk());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    EXPECT_EQ(tableOf(map)->item(0, 0)->text(), "Low, load");
    EXPECT_EQ(tableOf(map)->item(0, 1)->text(), "High");
}
