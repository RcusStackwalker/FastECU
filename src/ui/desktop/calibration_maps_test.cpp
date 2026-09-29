#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <string>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <QTableWidget>
#include <gtest/gtest.h>

#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/testing/fake_definition_catalogs.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/ui/desktop/calibration_maps.h"

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
        const auto initialized = cfg.initialize();
        if (!initialized.has_value())
        {
            return std::unexpected(initialized.error());
        }
        auto& settings = cfg.session.settings();
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
        cfg.put("/defs/maps.xml", xml);
        cfg.file_system.files["/defs/maps.xml"] = {};
        catalogs.entries[DefinitionFormat::EcuFlash] = {{.format = DefinitionFormat::EcuFlash,
                                                         .definition_id = "MAPS",
                                                         .internal_id = "MAPS",
                                                         .internal_id_address = 0,
                                                         .internal_id_encoding = fastecu::definition::IdEncoding::Ascii,
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
        const auto opened = workspace.open_file("/cal/maps.bin");
        if (!opened.has_value())
        {
            return std::unexpected(opened.error());
        }
        if (workspace.find(opened->id)->definition() == nullptr)
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "synthetic map definition did not resolve");
        }
        return opened->id;
    }
};

constexpr auto kXAxis = R"(<table type="X Axis" name="Speed" address="40" elements="2" scaling="Raw"/>)";
constexpr auto kYAxis = R"(<table type="Y Axis" name="Load" address="50" elements="2" scaling="Raw"/>)";

std::string numeric_table(std::string_view type, int x, int y, std::string_view axes = {})
{
    return "<table name=\"Timing\" type=\"" + std::string(type) + "\" address=\"20\" scaling=\"Raw\" sizex=\"" +
           std::to_string(x) + "\" sizey=\"" + std::to_string(y) + "\">" + std::string(axes) + "</table>";
}

void replace_definition(fastecu::calibration::CalibrationSession& session,
                        fastecu::calibration::ResolvedDefinition definition)
{
    const auto rom = session.rom();
    fastecu::calibration::SessionContents contents{.source = session.source(),
                                                   .rom = std::vector<std::uint8_t>(rom.begin(), rom.end()),
                                                   .definition = std::move(definition),
                                                   .protocol = session.protocol()};
    session = fastecu::calibration::CalibrationSession(session.id(), std::move(contents));
}

QTableWidget *table_of(CalibrationMaps& map)
{
    return map.findChild<QTableWidget *>();
}
} // namespace

class CalibrationMapsTest : public ::testing::Test
{

  public:
};

struct layoutsAndRefreshCase
{
    std::string name;
    QString type;
    int x;
    int y;
    int rows;
    int cols;
    int bodyRow;
    int bodyCol;
};
class layoutsAndRefreshParameters : public CalibrationMapsTest,
                                    public ::testing::WithParamInterface<layoutsAndRefreshCase>
{
};

INSTANTIATE_TEST_SUITE_P(Rows, layoutsAndRefreshParameters,
                         ::testing::Values(layoutsAndRefreshCase{"1D", "1D", 1, 1, 1, 1, 0, 0},
                                           layoutsAndRefreshCase{"X_2D", "2D", 2, 1, 2, 2, 1, 0},
                                           layoutsAndRefreshCase{"Y_2D", "2D", 1, 2, 2, 2, 0, 1},
                                           layoutsAndRefreshCase{"3D", "3D", 2, 2, 3, 3, 1, 1}),
                         [](const ::testing::TestParamInfo<layoutsAndRefreshCase>& info) { return info.param.name; });

TEST_P(layoutsAndRefreshParameters, layoutsAndRefresh)
{
    const QString type = GetParam().type;
    const int x = GetParam().x;
    const int y = GetParam().y;
    const int rows = GetParam().rows;
    const int cols = GetParam().cols;
    const int bodyRow = GetParam().bodyRow;
    const int bodyCol = GetParam().bodyCol;
    MapFixture fixture;
    const auto id = fixture.open(
        numeric_table(type.toStdString(), x, y,
                      (x > 1 ? std::string(kXAxis) : std::string{}) + (y > 1 ? std::string(kYAxis) : std::string{})));
    ASSERT_TRUE(id.has_value());
    if (type == "2D" && y > 1)
    {
        // EcuFlash normalizes a 2D Y axis into its X dimension. Supply
        // the legacy vertical resolved geometry the UI still supports.
        auto *session = fixture.workspace.find(*id);
        auto definition = *session->definition();
        auto& typedMap = definition.definition.maps[0];
        typedMap.x_size = 1;
        typedMap.y_size = 2;
        typedMap.y_axis = typedMap.x_axis;
        typedMap.x_axis = {};
        replace_definition(*session, std::move(definition));
    }
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = table_of(map);
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
    ASSERT_TRUE(fixture.workspace.find(*id)->write_bytes(32, edit).has_value());
    map.refresh();
    ASSERT_EQ(table->item(bodyRow, bodyCol)->text(), "55.0");
    ASSERT_EQ(changed.count(), 0);
    ASSERT_EQ(table->rowCount(), rows);
    ASSERT_EQ(table->columnCount(), cols);
}

struct staticAxisLabelsCase
{
    std::string name;
    QString axisType;
};
class staticAxisLabelsParameters : public CalibrationMapsTest,
                                   public ::testing::WithParamInterface<staticAxisLabelsCase>
{
};

INSTANTIATE_TEST_SUITE_P(Rows, staticAxisLabelsParameters,
                         ::testing::Values(staticAxisLabelsCase{"static_X", "Static X Axis"},
                                           staticAxisLabelsCase{"static_Y", "Static Y Axis"}),
                         [](const ::testing::TestParamInfo<staticAxisLabelsCase>& info) { return info.param.name; });

TEST_P(staticAxisLabelsParameters, staticAxisLabels)
{
    const QString axisType = GetParam().axisType;
    MapFixture fixture;
    const std::string axis = "<table type=\"" + std::string("Static X Axis") +
                             "\" name=\"Labels\" elements=\"2\"><data>Low</data><data>High</data></table>";
    const auto id = fixture.open(numeric_table("2D", 2, 1, axis));
    ASSERT_TRUE(id.has_value());
    if (axisType == "Static Y Axis")
    {
        // The retained UI accepts this legacy tag; the typed resolver
        // currently emits only Static X Axis, so supply its equivalent
        // resolved shape directly after opening the normal fixture.
        auto *session = fixture.workspace.find(*id);
        auto definition = *session->definition();
        definition.definition.maps[0].x_axis.type = "Static Y Axis";
        replace_definition(*session, std::move(definition));
    }
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = table_of(map);
    ASSERT_EQ(table->rowCount(), 2);
    ASSERT_EQ(table->columnCount(), 2);
    ASSERT_EQ(table->item(0, 0)->text(), "Low");
    ASSERT_EQ(table->item(0, 1)->text(), "High");
    const std::array<std::uint8_t, 1> edit{77};
    ASSERT_TRUE(fixture.workspace.find(*id)->write_bytes(32, edit).has_value());
    map.refresh();
    ASSERT_EQ(table->item(0, 0)->text(), "Low");
    ASSERT_EQ(table->item(1, 0)->text(), "77.0");
}

TEST_F(CalibrationMapsTest, absentAxisUsesSequentialFallback)
{
    MapFixture fixture;
    const auto id = fixture.open(numeric_table("2D", 2, 1));
    ASSERT_TRUE(id.has_value());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = table_of(map);
    ASSERT_EQ(table->item(0, 0)->text(), "0");
    ASSERT_EQ(table->item(0, 1)->text(), "1");
    map.refresh();
    ASSERT_EQ(table->item(0, 0)->text(), "0");
    ASSERT_EQ(table->item(0, 1)->text(), "1");
}

TEST_F(CalibrationMapsTest, selectableReflectsBlobBytesWithoutEmittingEditSignal)
{
    MapFixture fixture;
    const auto id = fixture.open(R"(<table name="Mode" type="Selectable" address="60" scaling="Modes"/>)");
    ASSERT_TRUE(id.has_value());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = table_of(map);
    auto *combo = qobject_cast<QComboBox *>(table->cellWidget(0, 0));
    ASSERT_TRUE(combo != nullptr);
    ASSERT_EQ(table->rowCount(), 1);
    ASSERT_EQ(table->columnCount(), 1);
    ASSERT_EQ(combo->count(), 2);
    ASSERT_EQ(combo->currentText(), "On");
    fastecu::testing::SignalRecorder edits(&map, &CalibrationMaps::selectable_combobox_item_changed);
    fastecu::testing::SignalRecorder changes(combo, &QComboBox::currentTextChanged);
    const std::array<std::uint8_t, 2> off{0, 0};
    ASSERT_TRUE(fixture.workspace.find(*id)->write_bytes(96, off).has_value());
    map.refresh();
    ASSERT_EQ(combo->currentText(), "Off");
    ASSERT_EQ(edits.count(), 0);
    ASSERT_EQ(changes.count(), 0);
    ASSERT_EQ(table->cellWidget(0, 0), combo);
}

TEST_F(CalibrationMapsTest, retainedMultiSelectableGeometryKeepsLegacyNumericCell)
{
    MapFixture fixture;
    const auto id = fixture.open(numeric_table("1D", 1, 1));
    ASSERT_TRUE(id.has_value());
    auto *session = fixture.workspace.find(*id);
    auto definition = *session->definition();
    auto& typedMap = definition.definition.maps[0];
    typedMap.type = "MultiSelectable";
    typedMap.y_axis.type = "Y Axis";
    typedMap.y_axis.name = "Labels";
    typedMap.y_axis.units = "Label";
    replace_definition(*session, std::move(definition));
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = table_of(map);
    ASSERT_EQ(table->rowCount(), 1);
    ASSERT_EQ(table->columnCount(), 1);
    ASSERT_TRUE(table->item(0, 0) != nullptr);
    ASSERT_EQ(table->item(0, 0)->text(), "10.0");
    const std::array<std::uint8_t, 1> edit{55};
    ASSERT_TRUE(session->write_bytes(32, edit).has_value());
    fastecu::testing::SignalRecorder changed(table, &QTableWidget::cellChanged);
    map.refresh();
    ASSERT_EQ(table->item(0, 0)->text(), "55.0");
    ASSERT_EQ(changed.count(), 0);
}

TEST_F(CalibrationMapsTest, retainedSwitchRefreshKeepsUncheckedControlWithoutEmittingEdits)
{
    MapFixture fixture;
    const auto id = fixture.open(numeric_table("1D", 1, 1));
    ASSERT_TRUE(id.has_value());
    auto *session = fixture.workspace.find(*id);
    auto definition = *session->definition();
    definition.definition.maps[0].type = "Switch";
    replace_definition(*session, std::move(definition));
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = table_of(map);
    ASSERT_EQ(table->rowCount(), 1);
    ASSERT_EQ(table->columnCount(), 1);
    auto *checkbox = qobject_cast<QCheckBox *>(table->cellWidget(0, 0));
    ASSERT_TRUE(checkbox != nullptr);
    ASSERT_TRUE(!checkbox->isChecked());
    fastecu::testing::SignalRecorder edits(&map, &CalibrationMaps::checkbox_state_changed);
    checkbox->setChecked(true);
    ASSERT_EQ(edits.count(), 1);
    const auto edits_before_refresh = edits.count();
    map.refresh();
    ASSERT_EQ(table->cellWidget(0, 0), checkbox);
    ASSERT_TRUE(!checkbox->isChecked());
    ASSERT_EQ(edits.count(), edits_before_refresh);
    ASSERT_TRUE(!session->dirty());
}

TEST_F(CalibrationMapsTest, colorsKeepOpeningBoundsDuringRefreshAndReopenUsesCurrentValues)
{
    MapFixture fixture;
    const auto id = fixture.open(numeric_table("2D", 2, 1, kXAxis));
    ASSERT_TRUE(id.has_value());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = table_of(map);
    const auto minimumColor = table->item(1, 0)->background().color();
    const auto maximumColor = table->item(1, 1)->background().color();
    ASSERT_TRUE(minimumColor != maximumColor);
    const std::array<std::uint8_t, 1> edit{30};
    ASSERT_TRUE(fixture.workspace.find(*id)->write_bytes(32, edit).has_value());
    map.refresh();
    ASSERT_EQ(table->item(1, 0)->background().color(), maximumColor);
    ASSERT_EQ(table->item(1, 1)->background().color(), maximumColor);
    CalibrationMaps reopened(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *reopenedTable = table_of(reopened);
    ASSERT_EQ(reopenedTable->item(1, 0)->background().color(), maximumColor);
    ASSERT_EQ(reopenedTable->item(1, 1)->background().color(), minimumColor);
    // Reopening must not alter the already open window's color bounds.
    map.refresh();
    ASSERT_EQ(table->item(1, 1)->background().color(), maximumColor);
}

TEST_F(CalibrationMapsTest, constantMapHasFiniteStableColors)
{
    MapFixture fixture;
    const auto id = fixture.open(numeric_table("2D", 2, 1, kXAxis), true);
    ASSERT_TRUE(id.has_value());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = table_of(map);
    const auto color = table->item(1, 0)->background().color();
    ASSERT_TRUE(color.isValid());
    ASSERT_EQ(table->item(1, 1)->background().color(), color);
    const std::array<std::uint8_t, 1> edit{30};
    ASSERT_TRUE(fixture.workspace.find(*id)->write_bytes(32, edit).has_value());
    map.refresh();
    ASSERT_EQ(table->item(1, 0)->background().color(), color);
    CalibrationMaps reopened(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    ASSERT_EQ(table_of(reopened)->item(1, 1)->background().color(), color);
}

TEST_F(CalibrationMapsTest, closedSessionRefreshIsInertAfterAnotherSessionOpens)
{
    MapFixture fixture;
    const auto id = fixture.open(numeric_table("1D", 1, 1));
    ASSERT_TRUE(id.has_value());
    CalibrationMaps map(fixture.workspace, *id, 0, QRect(0, 0, 800, 600));
    auto *table = table_of(map);
    fastecu::testing::SignalRecorder changed(table, &QTableWidget::cellChanged);
    ASSERT_TRUE(fixture.workspace.close(*id).has_value());
    const auto replacement = fixture.workspace.open_file("/cal/maps.bin");
    ASSERT_TRUE(replacement.has_value());
    ASSERT_TRUE(replacement->id != *id);
    const std::array<std::uint8_t, 1> edit{99};
    ASSERT_TRUE(fixture.workspace.find(replacement->id)->write_bytes(32, edit).has_value());
    map.refresh();
    ASSERT_EQ(table->item(0, 0)->text(), "10.0");
    ASSERT_EQ(changed.count(), 0);
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment);
}
