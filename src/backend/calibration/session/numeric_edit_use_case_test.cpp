#include "src/backend/calibration/session/numeric_edit_use_case.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::HasSubstr;
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

auto changed()
{
    return IsOkAnd(VariantWith<NumericEditChanged>(::testing::_));
}

auto unchanged(NoChangeReason reason)
{
    return IsOkAnd(VariantWith<NumericEditUnchanged>(Field(&NumericEditUnchanged::reason, reason)));
}

auto not_applicable(NotApplicableReason reason)
{
    return IsOkAnd(VariantWith<NumericEditNotApplicable>(Field(&NumericEditNotApplicable::reason, reason)));
}

class NumericEditUseCaseTest : public ::testing::Test
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

    std::vector<std::uint8_t> body_bytes()
    {
        const auto rom = rom_bytes();
        return {rom.begin() + static_cast<std::ptrdiff_t>(kBody), rom.begin() + static_cast<std::ptrdiff_t>(kBody + 6)};
    }

    Result<NumericEditOutcome> edit(NumericTarget target, SelectionRange elements, NumericEditOperation operation,
                                    std::size_t map_index = 0)
    {
        return apply_numeric_edit(workspace, {.session = id,
                                              .map_index = map_index,
                                              .selection = {.target = target, .elements = elements},
                                              .operation = std::move(operation)});
    }

    config::testing::ConfigSessionFixture cfg;
    InMemoryAtomicFileWriter writer;
    definition::DefinitionService definitions{cfg.file_system, cfg.file_repository, writer};
    testing::FakeDefinitionCatalogs catalogs;
    RomOpenUseCase opener{catalogs, definitions, cfg.file_repository, cfg.file_system, cfg.events, cfg.session};
    CalibrationWorkspace workspace{opener};
    SessionId id{};
};

TEST_F(NumericEditUseCaseTest, ChangesTheSelectedBodyCell)
{
    EXPECT_THAT(edit(NumericTarget::MapBody, cell(1, 1), IncrementEdit{IncrementStep::CoarseUp}), changed());

    EXPECT_THAT(body_bytes(), ElementsAre(10, 20, 30, 40, 60, 200));
    EXPECT_TRUE(session().dirty());
}

TEST_F(NumericEditUseCaseTest, EditsAxesWithTheirOwnGeometry)
{
    EXPECT_THAT(edit(NumericTarget::XAxis, cell(0, 2), IncrementEdit{IncrementStep::CoarseUp}), changed());
    EXPECT_THAT(edit(NumericTarget::YAxis, cell(1, 0), IncrementEdit{IncrementStep::FineUp}), changed());

    EXPECT_EQ(session().rom()[2], 13);
    EXPECT_EQ(session().rom()[9], 6);
    EXPECT_THAT(body_bytes(), ElementsAre(10, 20, 30, 40, 50, 200));
}

TEST_F(NumericEditUseCaseTest, CalculatesFromBytesWrittenBeforeTheCall)
{
    const std::array<std::uint8_t, 1> value{33};
    ASSERT_THAT(session().write_bytes(kBody, value), IsOk());

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), IncrementEdit{IncrementStep::FineUp}), changed());

    EXPECT_EQ(session().rom()[kBody], 34);
}

TEST_F(NumericEditUseCaseTest, AssignsAnExpressionAcrossARow)
{
    EXPECT_THAT(edit(NumericTarget::MapBody, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 2},
                     AssignmentEdit{"x*2"}),
                changed());

    EXPECT_THAT(body_bytes(), ElementsAre(20, 40, 60, 40, 50, 200));
}

TEST_F(NumericEditUseCaseTest, InterpolatesBetweenTheSelectionEdges)
{
    const std::array<std::uint8_t, 3> row{10, 0, 30};
    ASSERT_THAT(session().write_bytes(kBody, row), IsOk());

    EXPECT_THAT(edit(NumericTarget::MapBody, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 2},
                     InterpolationEdit{InterpolationMode::Horizontal}),
                changed());

    EXPECT_THAT(body_bytes(), ElementsAre(10, 20, 30, 40, 50, 200));
}

TEST_F(NumericEditUseCaseTest, RejectsSelectionsOutsideTheTargetRun)
{
    const auto before = rom_bytes();

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(2, 0), IncrementEdit{IncrementStep::FineUp}),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(edit(NumericTarget::YAxis, cell(0, 1), IncrementEdit{IncrementStep::FineUp}),
                IsErr(ErrorKind::InvalidConfig));
    // A drag from the header corner translates to a negative element row.
    EXPECT_THAT(edit(NumericTarget::MapBody, {.first_row = -1, .first_col = 0, .last_row = 1, .last_col = 2},
                     AssignmentEdit{"7"}),
                IsErr(ErrorKind::InvalidConfig));

    EXPECT_EQ(rom_bytes(), before);
    EXPECT_FALSE(session().dirty());
}

TEST_F(NumericEditUseCaseTest, RejectsAnInvalidExpressionWithoutMutation)
{
    const auto before = rom_bytes();

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), AssignmentEdit{"x+"}), IsErr(ErrorKind::InvalidConfig));

    EXPECT_EQ(rom_bytes(), before);
    EXPECT_FALSE(session().dirty());
}

TEST_F(NumericEditUseCaseTest, AFailingCellRejectsTheWholeSelection)
{
    auto def = grid_definition();
    def.scalings[0].maximum.clear();
    auto rom = grid_rom();
    rom[kBody + 5] = 250;
    install(std::move(def), rom);

    EXPECT_THAT(edit(NumericTarget::MapBody, {.first_row = 1, .first_col = 1, .last_row = 1, .last_col = 2},
                     IncrementEdit{IncrementStep::CoarseUp}),
                IsErrWith(ErrorKind::InvalidConfig, HasSubstr("storage range")));

    EXPECT_EQ(rom_bytes(), rom);
    EXPECT_FALSE(session().dirty());
}

TEST_F(NumericEditUseCaseTest, AFailedEditKeepsAnAlreadyDirtySessionDirty)
{
    auto def = grid_definition();
    def.scalings[0].maximum.clear();
    auto rom = grid_rom();
    rom[kBody + 5] = 250;
    install(std::move(def), rom);
    const std::array<std::uint8_t, 1> unrelated{7};
    ASSERT_THAT(session().write_bytes(0x1F, unrelated), IsOk());
    const auto before = rom_bytes();

    EXPECT_THAT(edit(NumericTarget::MapBody, {.first_row = 1, .first_col = 1, .last_row = 1, .last_col = 2},
                     IncrementEdit{IncrementStep::CoarseUp}),
                IsErr(ErrorKind::InvalidConfig));

    EXPECT_EQ(rom_bytes(), before);
    EXPECT_TRUE(session().dirty());
}

TEST_F(NumericEditUseCaseTest, RelativeEditsNeedValidCurrentValuesButAssignmentsDoNot)
{
    auto def = grid_definition();
    def.scalings[0].from_byte = "1/0";
    install(std::move(def));

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), AssignmentEdit{"x+1"}), IsErr(ErrorKind::InvalidConfig));
    EXPECT_FALSE(session().dirty());

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), AssignmentEdit{"5"}), changed());
    EXPECT_EQ(session().rom()[kBody], 5);
}

TEST_F(NumericEditUseCaseTest, ReportsLimitAndOrdinaryNoChangeWithoutDirtying)
{
    const auto before = rom_bytes();

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(1, 2), IncrementEdit{IncrementStep::CoarseUp}),
                unchanged(NoChangeReason::DefinitionLimit));
    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), AssignmentEdit{"x"}), unchanged(NoChangeReason::Unchanged));

    EXPECT_EQ(rom_bytes(), before);
    EXPECT_FALSE(session().dirty());
}

TEST_F(NumericEditUseCaseTest, ReportsStorageResolutionAndCombinedCauses)
{
    auto def = grid_definition();
    def.scalings[0].from_byte = "x/10";
    def.scalings[0].to_byte = "x*10";
    def.scalings[0].maximum = "20";
    def.scalings[0].fine_increment = "0.01";
    install(std::move(def));

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), IncrementEdit{IncrementStep::FineUp}),
                unchanged(NoChangeReason::BelowStorageResolution));
    EXPECT_THAT(edit(NumericTarget::MapBody, {.first_row = 1, .first_col = 1, .last_row = 1, .last_col = 2},
                     IncrementEdit{IncrementStep::FineUp}),
                unchanged(NoChangeReason::MultipleCauses));
    EXPECT_FALSE(session().dirty());
}

TEST_F(NumericEditUseCaseTest, ACompleteNoOpKeepsAnAlreadyDirtySessionDirty)
{
    const std::array<std::uint8_t, 1> unrelated{7};
    ASSERT_THAT(session().write_bytes(0x1F, unrelated), IsOk());

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), AssignmentEdit{"x"}), unchanged(NoChangeReason::Unchanged));

    EXPECT_TRUE(session().dirty());
}

TEST_F(NumericEditUseCaseTest, BodyStorageAndEndianFallBackToTheScaling)
{
    auto def = grid_definition();
    auto& map = def.maps[0];
    map.type = "2D";
    map.x_size = 2;
    map.y_size = 1;
    map.storage_type.reset();
    map.endian.clear();
    map.x_axis = {};
    map.y_axis = {};
    def.scalings[0].storage_type = definition::StorageType::Uint16;
    def.scalings[0].endian = "little";
    auto rom = grid_rom();
    const std::array<std::uint8_t, 4> body{0x10, 0x00, 0x20, 0x00};
    std::ranges::copy(body, rom.begin() + static_cast<std::ptrdiff_t>(kBody));
    install(std::move(def), rom);

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), AssignmentEdit{"x+1"}), changed());

    EXPECT_THAT(body_bytes(), ElementsAre(0x11, 0x00, 0x20, 0x00, 50, 200));
}

TEST_F(NumericEditUseCaseTest, Wrx02WritesKeepThePreservedRelocation)
{
    definition::RomDefinition def = grid_definition();
    auto& map = def.maps[0];
    map.type = "1D";
    map.address = 0x2C000;
    map.x_size = 1;
    map.y_size = 1;
    map.x_axis = {};
    map.y_axis = {};
    std::vector<std::uint8_t> rom(0x30000, 0);
    rom[0x2C000] = 10;
    install(std::move(def), rom, RomProtocolInfo{.flash_method = "wrx02", .unpadded_size = 0x28000});

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), AssignmentEdit{"20"}), changed());

    // The evidence-gated write predicate relocates below 190 KiB images.
    EXPECT_EQ(session().rom()[0x24000], 20);
    EXPECT_EQ(session().rom()[0x2C000], 10);
}

TEST_F(NumericEditUseCaseTest, StaleSessionIdsAreNotApplicable)
{
    const auto before = rom_bytes();
    EXPECT_THAT(apply_numeric_edit(workspace, {.session = SessionId{999},
                                               .map_index = 0,
                                               .selection = {.target = NumericTarget::MapBody, .elements = cell(0, 0)},
                                               .operation = AssignmentEdit{"7"}}),
                not_applicable(NotApplicableReason::ClosedSession));
    EXPECT_EQ(rom_bytes(), before);

    ASSERT_THAT(workspace.close(id), IsOk());
    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), AssignmentEdit{"7"}),
                not_applicable(NotApplicableReason::ClosedSession));
}

TEST_F(NumericEditUseCaseTest, ASessionWithoutADefinitionIsNotApplicable)
{
    install(std::nullopt);

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), AssignmentEdit{"7"}),
                not_applicable(NotApplicableReason::NoDefinition));
    EXPECT_FALSE(session().dirty());
}

TEST_F(NumericEditUseCaseTest, UnavailableTargetsAreNotApplicable)
{
    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), AssignmentEdit{"7"}, 1),
                not_applicable(NotApplicableReason::UnavailableTarget));

    auto static_axis = grid_definition();
    static_axis.maps[0].x_axis = {.type = "Static X Axis", .static_data = {"a", "b", "c"}};
    static_axis.maps[0].y_axis = {};
    install(std::move(static_axis));
    EXPECT_THAT(edit(NumericTarget::XAxis, cell(0, 0), AssignmentEdit{"7"}),
                not_applicable(NotApplicableReason::UnavailableTarget));
    EXPECT_THAT(edit(NumericTarget::YAxis, cell(0, 0), AssignmentEdit{"7"}),
                not_applicable(NotApplicableReason::UnavailableTarget));

    auto structural = grid_definition();
    structural.maps[0].address.reset();
    install(std::move(structural));
    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), AssignmentEdit{"7"}),
                not_applicable(NotApplicableReason::UnavailableTarget));

    EXPECT_THAT(rom_bytes(), ::testing::Eq(grid_rom()));
    EXPECT_FALSE(session().dirty());
}

TEST_F(NumericEditUseCaseTest, PastesRowsFromTheSelectionTopLeft)
{
    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 1), PasteEdit{{{"1", "2"}, {"3", "4"}}}), changed());

    EXPECT_THAT(body_bytes(), ElementsAre(10, 1, 2, 40, 3, 4));
}

TEST_F(NumericEditUseCaseTest, ClipsALargerSourceToTheRunEdges)
{
    EXPECT_THAT(
        edit(NumericTarget::MapBody, cell(1, 1), PasteEdit{{{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}}}),
        changed());

    EXPECT_THAT(body_bytes(), ElementsAre(10, 20, 30, 40, 1, 2));
    const auto rom = rom_bytes();
    EXPECT_THAT(std::vector<std::uint8_t>(rom.begin(), rom.begin() + 3), ElementsAre(1, 2, 3));
}

TEST_F(NumericEditUseCaseTest, RaggedRowsPasteWhatEachRowSupplies)
{
    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), PasteEdit{{{"1", "2"}, {"3"}}}), changed());

    EXPECT_THAT(body_bytes(), ElementsAre(1, 2, 30, 3, 50, 200));
}

TEST_F(NumericEditUseCaseTest, EverySuppliedCellIsValidatedBeforeClipping)
{
    const auto before = rom_bytes();

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), PasteEdit{{{"1", "2", "3", "x"}}}),
                IsErrWith(ErrorKind::InvalidConfig, HasSubstr("invalid numeric literal")));
    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), PasteEdit{{{"1"}, {"2"}, {"x"}}}),
                IsErrWith(ErrorKind::InvalidConfig, HasSubstr("invalid numeric literal")));
    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), PasteEdit{{{"1"}, {""}, {"3"}}}),
                IsErrWith(ErrorKind::InvalidConfig, HasSubstr("empty")));

    EXPECT_EQ(rom_bytes(), before);
    EXPECT_FALSE(session().dirty());
}

TEST_F(NumericEditUseCaseTest, PasteAcceptsCarriageReturnsAndOnlyDotDecimals)
{
    EXPECT_THAT(edit(NumericTarget::MapBody, cell(1, 0), PasteEdit{{{"1,5"}}}), IsErr(ErrorKind::InvalidConfig));
    EXPECT_FALSE(session().dirty());

    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), PasteEdit{{{"7\r", "1.6"}}}), changed());

    EXPECT_THAT(body_bytes(), ElementsAre(7, 2, 30, 40, 50, 200));
}

TEST_F(NumericEditUseCaseTest, AxesUseTheirOwnGeometry)
{
    EXPECT_THAT(edit(NumericTarget::XAxis, cell(0, 1), PasteEdit{{{"7", "8"}, {"9", "9"}}}), changed());
    EXPECT_THAT(edit(NumericTarget::YAxis, cell(0, 0), PasteEdit{{{"6", "9"}, {"7", "9"}}}), changed());

    const auto rom = rom_bytes();
    EXPECT_THAT(std::vector<std::uint8_t>(rom.begin(), rom.begin() + 3), ElementsAre(1, 7, 8));
    EXPECT_THAT(std::vector<std::uint8_t>(rom.begin() + 8, rom.begin() + 10), ElementsAre(6, 7));
    EXPECT_THAT(body_bytes(), ElementsAre(10, 20, 30, 40, 50, 200));
}

TEST_F(NumericEditUseCaseTest, PastingCurrentValuesIsANoOp)
{
    EXPECT_THAT(edit(NumericTarget::MapBody, cell(0, 0), PasteEdit{{{"10", "20"}}}),
                unchanged(NoChangeReason::Unchanged));
    EXPECT_FALSE(session().dirty());
}

} // namespace
} // namespace fastecu::calibration
