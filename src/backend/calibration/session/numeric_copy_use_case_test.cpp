#include "src/backend/calibration/session/numeric_copy_use_case.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/calibration/session/testing/fake_definition_catalogs.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration
{
namespace
{

using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::Field;
using ::testing::VariantWith;

constexpr std::size_t kBody = 0x10;

// A 3x2 uint8 body at 0x10 with identity scaling bounded to [0, 200], a
// three-element X axis at 0x00 and a two-element Y axis at 0x08.
definition::RomDefinition grid_definition()
{
    definition::RomDefinition rom{.format = definition::DefinitionFormat::EcuFlash};
    rom.scalings.push_back(definition::Scaling{.name = "raw",
                                               .from_byte = "x",
                                               .to_byte = "x",
                                               .minimum = "0",
                                               .maximum = "200",
                                               .coarse_increment = "10",
                                               .fine_increment = "1"});
    definition::CalibrationMap map;
    map.name = "Grid";
    map.type = "3D";
    map.address = kBody;
    map.x_size = 3;
    map.y_size = 2;
    map.storage_type = definition::StorageType::Uint8;
    map.endian = "big";
    map.scaling_name = "raw";
    map.x_axis.type = "X Axis";
    map.x_axis.address = 0x00;
    map.x_axis.storage_type = definition::StorageType::Uint8;
    map.x_axis.scaling_name = "raw";
    map.y_axis.type = "Y Axis";
    map.y_axis.address = 0x08;
    map.y_axis.storage_type = definition::StorageType::Uint8;
    map.y_axis.scaling_name = "raw";
    rom.maps.push_back(map);
    return rom;
}

// Body row-major: {10, 20, 30} / {40, 50, 200}; the last cell sits on the limit.
std::vector<std::uint8_t> grid_rom()
{
    std::vector<std::uint8_t> rom(0x20, 0);
    rom[0] = 1;
    rom[1] = 2;
    rom[2] = 3;
    rom[8] = 4;
    rom[9] = 5;
    const std::array<std::uint8_t, 6> body{10, 20, 30, 40, 50, 200};
    std::ranges::copy(body, rom.begin() + static_cast<std::ptrdiff_t>(kBody));
    return rom;
}

SelectionRange cell(int row, int col)
{
    return {.first_row = row, .first_col = col, .last_row = row, .last_col = col};
}

auto not_applicable(NotApplicableReason reason)
{
    return IsOkAnd(VariantWith<NumericEditNotApplicable>(Field(&NumericEditNotApplicable::reason, reason)));
}

class NumericCopyUseCaseTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg.initialize(), IsOk());
        const auto opened = workspace.adopt_read_image(
            ReadImage{.rom = std::vector<std::uint8_t>(8, 0), .filename = "grid.bin", .protocol_name = "proto_b"});
        ASSERT_THAT(opened, IsOk());
        id = opened->id;
        install(grid_definition());
    }

    // Replaces the open session's contents with a clean session.
    void install(std::optional<definition::RomDefinition> def, std::vector<std::uint8_t> rom = grid_rom(),
                 RomProtocolInfo protocol = {})
    {
        std::optional<ResolvedDefinition> resolved;
        if (def.has_value())
        {
            resolved = ResolvedDefinition{.definition = std::move(*def)};
        }
        *workspace.find(id) = CalibrationSession(id, SessionContents{.source = {.display_name = "grid.bin"},
                                                                     .rom = std::move(rom),
                                                                     .definition = std::move(resolved),
                                                                     .protocol = std::move(protocol)});
    }

    CalibrationSession& session()
    {
        return *workspace.find(id);
    }

    std::vector<std::uint8_t> rom_bytes()
    {
        const auto rom = session().rom();
        return {rom.begin(), rom.end()};
    }

    Result<NumericCopyOutcome> copy(NumericTarget target, SelectionRange elements, std::size_t map_index = 0)
    {
        return copy_numeric_values(
            workspace, {.session = id, .map_index = map_index, .selection = {.target = target, .elements = elements}});
    }

    config::testing::ConfigSessionFixture cfg;
    InMemoryAtomicFileWriter writer;
    definition::DefinitionService definitions{cfg.file_system, cfg.file_repository, writer};
    testing::FakeDefinitionCatalogs catalogs;
    RomOpenUseCase opener{catalogs, definitions, cfg.file_repository, cfg.file_system, cfg.events, cfg.session};
    CalibrationWorkspace workspace{opener};
    SessionId id{};
};

auto copied_text(const std::string& text)
{
    return IsOkAnd(VariantWith<NumericCopyText>(Field(&NumericCopyText::text, text)));
}

TEST_F(NumericCopyUseCaseTest, CopiesTheWholeBodyRowMajorWithTabsAndLineFeeds)
{
    EXPECT_THAT(copy(NumericTarget::MapBody, {.first_row = 0, .first_col = 0, .last_row = 1, .last_col = 2}),
                copied_text("10\t20\t30\n40\t50\t200"));
}

TEST_F(NumericCopyUseCaseTest, CopiesASubRangeFromItsTopLeft)
{
    EXPECT_THAT(copy(NumericTarget::MapBody, {.first_row = 0, .first_col = 1, .last_row = 1, .last_col = 2}),
                copied_text("20\t30\n50\t200"));
    EXPECT_THAT(copy(NumericTarget::MapBody, cell(1, 0)), copied_text("40"));
}

TEST_F(NumericCopyUseCaseTest, CopiesAxesWithTheirOwnGeometry)
{
    EXPECT_THAT(copy(NumericTarget::XAxis, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 2}),
                copied_text("1\t2\t3"));
    EXPECT_THAT(copy(NumericTarget::YAxis, {.first_row = 0, .first_col = 0, .last_row = 1, .last_col = 0}),
                copied_text("4\n5"));
}

// The copied text is the scaled value, not what a display format would show.
TEST_F(NumericCopyUseCaseTest, CopiesFullPrecisionScaledValuesNotRoundedDisplayText)
{
    auto def = grid_definition();
    def.scalings[0].from_byte = "x/3";
    def.scalings[0].to_byte = "x*3";
    def.scalings[0].format = "%.1f";
    install(std::move(def));

    const auto copied = copy(NumericTarget::MapBody, cell(0, 0));

    ASSERT_THAT(copied, IsOk());
    const auto *text = std::get_if<NumericCopyText>(&*copied);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(std::strtod(text->text.c_str(), nullptr), 10.0 / 3.0);
    EXPECT_GT(text->text.size(), std::string("3.3").size());
}

TEST_F(NumericCopyUseCaseTest, WritesPlainDecimalNeverExponentNotation)
{
    auto def = grid_definition();
    def.scalings[0].from_byte = "x/1000000";
    def.scalings[0].to_byte = "x*1000000";
    install(std::move(def));

    const auto copied = copy(NumericTarget::MapBody, cell(0, 0));

    ASSERT_THAT(copied, IsOk());
    const auto *text = std::get_if<NumericCopyText>(&*copied);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text.find_first_of("eE"), std::string::npos);
    EXPECT_EQ(std::strtod(text->text.c_str(), nullptr), 10.0 / 1000000.0);
}

TEST_F(NumericCopyUseCaseTest, PastingTheCopiedTextBackIsAnUnchangedEdit)
{
    auto def = grid_definition();
    def.scalings[0].from_byte = "x/3";
    def.scalings[0].to_byte = "x*3";
    install(std::move(def));
    const SelectionRange all{.first_row = 0, .first_col = 0, .last_row = 1, .last_col = 2};
    const auto copied = copy(NumericTarget::MapBody, all);
    ASSERT_THAT(copied, IsOk());
    const auto *text = std::get_if<NumericCopyText>(&*copied);
    ASSERT_NE(text, nullptr);
    PasteEdit paste;
    std::vector<std::string> row;
    std::string cell_text;
    for (const char c : text->text)
    {
        if (c == '\t' || c == '\n')
        {
            row.push_back(std::exchange(cell_text, {}));
            if (c == '\n')
            {
                paste.rows.push_back(std::exchange(row, {}));
            }
            continue;
        }
        cell_text += c;
    }
    row.push_back(cell_text);
    paste.rows.push_back(row);

    const auto pasted = apply_numeric_edit(workspace, {.session = id,
                                                       .map_index = 0,
                                                       .selection = {.target = NumericTarget::MapBody, .elements = all},
                                                       .operation = paste});

    EXPECT_THAT(pasted, IsOkAnd(VariantWith<NumericEditUnchanged>(::testing::_)));
}

TEST_F(NumericCopyUseCaseTest, ReportsTheFirstInvalidCellAndNoText)
{
    auto def = grid_definition();
    def.scalings[0].from_byte = "1/0";
    install(std::move(def));

    const auto copied = copy(NumericTarget::MapBody, {.first_row = 0, .first_col = 1, .last_row = 1, .last_col = 2});

    ASSERT_THAT(copied, IsOk());
    const auto *invalid = std::get_if<NumericCopyInvalidCell>(&*copied);
    ASSERT_NE(invalid, nullptr);
    EXPECT_EQ(invalid->row, 0);
    EXPECT_EQ(invalid->col, 1);
    EXPECT_FALSE(invalid->detail.empty());
}

TEST_F(NumericCopyUseCaseTest, CopyNeverChangesTheSession)
{
    const auto before = rom_bytes();

    EXPECT_THAT(copy(NumericTarget::MapBody, cell(0, 0)), IsOk());

    EXPECT_EQ(rom_bytes(), before);
    EXPECT_FALSE(session().dirty());
}

TEST_F(NumericCopyUseCaseTest, IsNotApplicableWithoutAUsableTarget)
{
    EXPECT_THAT(
        copy_numeric_values(workspace, {.session = SessionId{9999},
                                        .selection = {.target = NumericTarget::MapBody, .elements = cell(0, 0)}}),
        not_applicable(NotApplicableReason::ClosedSession));
    EXPECT_THAT(copy(NumericTarget::MapBody, cell(0, 0), 5), not_applicable(NotApplicableReason::UnavailableTarget));

    install(std::nullopt);
    EXPECT_THAT(copy(NumericTarget::MapBody, cell(0, 0)), not_applicable(NotApplicableReason::NoDefinition));
}

TEST_F(NumericCopyUseCaseTest, RejectsSelectionsOutsideTheRun)
{
    EXPECT_THAT(copy(NumericTarget::MapBody, cell(2, 0)), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(copy(NumericTarget::MapBody, cell(0, 3)), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(copy(NumericTarget::MapBody, cell(-1, 0)), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(copy(NumericTarget::MapBody, {.first_row = 1, .first_col = 0, .last_row = 0, .last_col = 0}),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(copy(NumericTarget::XAxis, cell(1, 0)), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(copy(NumericTarget::YAxis, cell(0, 1)), IsErr(ErrorKind::InvalidConfig));
}

} // namespace
} // namespace fastecu::calibration
