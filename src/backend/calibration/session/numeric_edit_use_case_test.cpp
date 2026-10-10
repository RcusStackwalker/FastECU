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
definition::RomDefinition GridDefinition()
{
    definition::RomDefinition rom{.format = definition::DefinitionFormat::kEcuFlash};
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
    map.storage_type = definition::StorageType::kUint8;
    map.endian = "big";
    map.scaling_name = "raw";
    map.x_axis.type = "X Axis";
    map.x_axis.address = 0x00;
    map.x_axis.storage_type = definition::StorageType::kUint8;
    map.x_axis.scaling_name = "raw";
    map.y_axis.type = "Y Axis";
    map.y_axis.address = 0x08;
    map.y_axis.storage_type = definition::StorageType::kUint8;
    map.y_axis.scaling_name = "raw";
    rom.maps.push_back(map);
    return rom;
}

// Body row-major: {10, 20, 30} / {40, 50, 200}; the last cell sits on the limit.
std::vector<std::uint8_t> GridRom()
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

SelectionRange Cell(int row, int col)
{
    return {.first_row = row, .first_col = col, .last_row = row, .last_col = col};
}

auto Changed()
{
    return IsOkAnd(VariantWith<NumericEditChanged>(::testing::_));
}

auto Unchanged(NoChangeReason reason)
{
    return IsOkAnd(VariantWith<NumericEditUnchanged>(Field(&NumericEditUnchanged::reason, reason)));
}

auto NotApplicable(NotApplicableReason reason)
{
    return IsOkAnd(VariantWith<NumericEditNotApplicable>(Field(&NumericEditNotApplicable::reason, reason)));
}

class NumericEditUseCaseTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg_.Initialize(), IsOk());
        const auto opened = workspace_.AdoptReadImage(
            ReadImage{.rom = std::vector<std::uint8_t>(8, 0), .filename = "grid.bin", .protocol_name = "proto_b"});
        ASSERT_THAT(opened, IsOk());
        id_ = opened->id;
        Install(GridDefinition());
    }

    // Replaces the open session's contents with a clean session.
    void Install(std::optional<definition::RomDefinition> def, std::vector<std::uint8_t> rom = GridRom(),
                 RomProtocolInfo protocol = {})
    {
        std::optional<ResolvedDefinition> resolved;
        if (def.has_value())
        {
            resolved = ResolvedDefinition{.definition = std::move(*def)};
        }
        *workspace_.Find(id_) = CalibrationSession(id_, SessionContents{.source = {.display_name = "grid.bin"},
                                                                        .rom = std::move(rom),
                                                                        .definition = std::move(resolved),
                                                                        .protocol = std::move(protocol)});
    }

    CalibrationSession& Session()
    {
        return *workspace_.Find(id_);
    }

    std::vector<std::uint8_t> RomBytes()
    {
        const auto rom = Session().Rom();
        return {rom.begin(), rom.end()};
    }

    std::vector<std::uint8_t> BodyBytes()
    {
        const auto rom = RomBytes();
        return {rom.begin() + static_cast<std::ptrdiff_t>(kBody), rom.begin() + static_cast<std::ptrdiff_t>(kBody + 6)};
    }

    Result<NumericEditOutcome> Edit(NumericTarget target, SelectionRange elements, NumericEditOperation operation,
                                    std::size_t map_index = 0)
    {
        return ApplyNumericEdit(workspace_, {.session = id_,
                                             .map_index = map_index,
                                             .selection = {.target = target, .elements = elements},
                                             .operation = std::move(operation)});
    }

    config::testing::ConfigSessionFixture cfg_;
    InMemoryAtomicFileWriter writer_;
    definition::DefinitionService definitions_{cfg_.file_system, cfg_.file_repository, writer_};
    testing::FakeDefinitionCatalogs catalogs_;
    RomOpenUseCase opener_{catalogs_, definitions_, cfg_.file_repository, cfg_.file_system, cfg_.events, cfg_.session};
    CalibrationWorkspace workspace_{opener_};
    SessionId id_{};
};

TEST_F(NumericEditUseCaseTest, ChangesTheSelectedBodyCell)
{
    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(1, 1), IncrementEdit{IncrementStep::kCoarseUp}), Changed());

    EXPECT_THAT(BodyBytes(), ElementsAre(10, 20, 30, 40, 60, 200));
    EXPECT_TRUE(Session().Dirty());
}

TEST_F(NumericEditUseCaseTest, EditsAxesWithTheirOwnGeometry)
{
    EXPECT_THAT(Edit(NumericTarget::kXAxis, Cell(0, 2), IncrementEdit{IncrementStep::kCoarseUp}), Changed());
    EXPECT_THAT(Edit(NumericTarget::kYAxis, Cell(1, 0), IncrementEdit{IncrementStep::kFineUp}), Changed());

    EXPECT_EQ(Session().Rom()[2], 13);
    EXPECT_EQ(Session().Rom()[9], 6);
    EXPECT_THAT(BodyBytes(), ElementsAre(10, 20, 30, 40, 50, 200));
}

TEST_F(NumericEditUseCaseTest, CalculatesFromBytesWrittenBeforeTheCall)
{
    const std::array<std::uint8_t, 1> value{33};
    ASSERT_THAT(Session().WriteBytes(kBody, value), IsOk());

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), IncrementEdit{IncrementStep::kFineUp}), Changed());

    EXPECT_EQ(Session().Rom()[kBody], 34);
}

TEST_F(NumericEditUseCaseTest, AssignsAnExpressionAcrossARow)
{
    EXPECT_THAT(Edit(NumericTarget::kMapBody, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 2},
                     AssignmentEdit{"x*2"}),
                Changed());

    EXPECT_THAT(BodyBytes(), ElementsAre(20, 40, 60, 40, 50, 200));
}

TEST_F(NumericEditUseCaseTest, InterpolatesBetweenTheSelectionEdges)
{
    const std::array<std::uint8_t, 3> row{10, 0, 30};
    ASSERT_THAT(Session().WriteBytes(kBody, row), IsOk());

    EXPECT_THAT(Edit(NumericTarget::kMapBody, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 2},
                     InterpolationEdit{InterpolationMode::kHorizontal}),
                Changed());

    EXPECT_THAT(BodyBytes(), ElementsAre(10, 20, 30, 40, 50, 200));
}

TEST_F(NumericEditUseCaseTest, RejectsSelectionsOutsideTheTargetRun)
{
    const auto before = RomBytes();

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(2, 0), IncrementEdit{IncrementStep::kFineUp}),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(Edit(NumericTarget::kYAxis, Cell(0, 1), IncrementEdit{IncrementStep::kFineUp}),
                IsErr(ErrorKind::kInvalidConfig));
    // A drag from the header corner translates to a negative element row.
    EXPECT_THAT(Edit(NumericTarget::kMapBody, {.first_row = -1, .first_col = 0, .last_row = 1, .last_col = 2},
                     AssignmentEdit{"7"}),
                IsErr(ErrorKind::kInvalidConfig));

    EXPECT_EQ(RomBytes(), before);
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(NumericEditUseCaseTest, RejectsAnInvalidExpressionWithoutMutation)
{
    const auto before = RomBytes();

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), AssignmentEdit{"x+"}), IsErr(ErrorKind::kInvalidConfig));

    EXPECT_EQ(RomBytes(), before);
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(NumericEditUseCaseTest, AFailingCellRejectsTheWholeSelection)
{
    auto def = GridDefinition();
    def.scalings[0].maximum.clear();
    auto rom = GridRom();
    rom[kBody + 5] = 250;
    Install(std::move(def), rom);

    EXPECT_THAT(Edit(NumericTarget::kMapBody, {.first_row = 1, .first_col = 1, .last_row = 1, .last_col = 2},
                     IncrementEdit{IncrementStep::kCoarseUp}),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("storage range")));

    EXPECT_EQ(RomBytes(), rom);
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(NumericEditUseCaseTest, AFailedEditKeepsAnAlreadyDirtySessionDirty)
{
    auto def = GridDefinition();
    def.scalings[0].maximum.clear();
    auto rom = GridRom();
    rom[kBody + 5] = 250;
    Install(std::move(def), rom);
    const std::array<std::uint8_t, 1> unrelated{7};
    ASSERT_THAT(Session().WriteBytes(0x1F, unrelated), IsOk());
    const auto before = RomBytes();

    EXPECT_THAT(Edit(NumericTarget::kMapBody, {.first_row = 1, .first_col = 1, .last_row = 1, .last_col = 2},
                     IncrementEdit{IncrementStep::kCoarseUp}),
                IsErr(ErrorKind::kInvalidConfig));

    EXPECT_EQ(RomBytes(), before);
    EXPECT_TRUE(Session().Dirty());
}

TEST_F(NumericEditUseCaseTest, RelativeEditsNeedValidCurrentValuesButAssignmentsDoNot)
{
    auto def = GridDefinition();
    def.scalings[0].from_byte = "1/0";
    Install(std::move(def));

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), AssignmentEdit{"x+1"}), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_FALSE(Session().Dirty());

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), AssignmentEdit{"5"}), Changed());
    EXPECT_EQ(Session().Rom()[kBody], 5);
}

TEST_F(NumericEditUseCaseTest, ReportsLimitAndOrdinaryNoChangeWithoutDirtying)
{
    const auto before = RomBytes();

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(1, 2), IncrementEdit{IncrementStep::kCoarseUp}),
                Unchanged(NoChangeReason::kDefinitionLimit));
    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), AssignmentEdit{"x"}), Unchanged(NoChangeReason::kUnchanged));

    EXPECT_EQ(RomBytes(), before);
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(NumericEditUseCaseTest, ReportsStorageResolutionAndCombinedCauses)
{
    auto def = GridDefinition();
    def.scalings[0].from_byte = "x/10";
    def.scalings[0].to_byte = "x*10";
    def.scalings[0].maximum = "20";
    def.scalings[0].fine_increment = "0.01";
    Install(std::move(def));

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), IncrementEdit{IncrementStep::kFineUp}),
                Unchanged(NoChangeReason::kBelowStorageResolution));
    EXPECT_THAT(Edit(NumericTarget::kMapBody, {.first_row = 1, .first_col = 1, .last_row = 1, .last_col = 2},
                     IncrementEdit{IncrementStep::kFineUp}),
                Unchanged(NoChangeReason::kMultipleCauses));
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(NumericEditUseCaseTest, ACompleteNoOpKeepsAnAlreadyDirtySessionDirty)
{
    const std::array<std::uint8_t, 1> unrelated{7};
    ASSERT_THAT(Session().WriteBytes(0x1F, unrelated), IsOk());

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), AssignmentEdit{"x"}), Unchanged(NoChangeReason::kUnchanged));

    EXPECT_TRUE(Session().Dirty());
}

TEST_F(NumericEditUseCaseTest, BodyStorageAndEndianFallBackToTheScaling)
{
    auto def = GridDefinition();
    auto& map = def.maps[0];
    map.type = "2D";
    map.x_size = 2;
    map.y_size = 1;
    map.storage_type.reset();
    map.endian.clear();
    map.x_axis = {};
    map.y_axis = {};
    def.scalings[0].storage_type = definition::StorageType::kUint16;
    def.scalings[0].endian = "little";
    auto rom = GridRom();
    const std::array<std::uint8_t, 4> body{0x10, 0x00, 0x20, 0x00};
    std::ranges::copy(body, rom.begin() + static_cast<std::ptrdiff_t>(kBody));
    Install(std::move(def), rom);

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), AssignmentEdit{"x+1"}), Changed());

    EXPECT_THAT(BodyBytes(), ElementsAre(0x11, 0x00, 0x20, 0x00, 50, 200));
}

// A real HC16Y5 `_02` session holds the padded 0x30000-byte image under its
// canonical protocol name (RomOpenUseCase resolves the `wrx02` alias before
// any edit), so a definition address at or above 0x28000 is written in place.
TEST_F(NumericEditUseCaseTest, Mc68PaddedImagesWriteHighAddressesInPlace)
{
    definition::RomDefinition def = GridDefinition();
    auto& map = def.maps[0];
    map.type = "1D";
    map.address = 0x2C000;
    map.x_size = 1;
    map.y_size = 1;
    map.x_axis = {};
    map.y_axis = {};
    std::vector<std::uint8_t> rom(0x30000, 0);
    rom[0x2C000] = 10;
    Install(std::move(def), rom, RomProtocolInfo{.flash_method = "sub_ecu_denso_mc68hc16y5_02"});

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), AssignmentEdit{"20"}), Changed());

    EXPECT_EQ(Session().Rom()[0x2C000], 20);
    EXPECT_EQ(Session().Rom()[0x24000], 0);
}

TEST_F(NumericEditUseCaseTest, StaleSessionIdsAreNotApplicable)
{
    const auto before = RomBytes();
    EXPECT_THAT(ApplyNumericEdit(workspace_, {.session = SessionId{999},
                                              .map_index = 0,
                                              .selection = {.target = NumericTarget::kMapBody, .elements = Cell(0, 0)},
                                              .operation = AssignmentEdit{"7"}}),
                NotApplicable(NotApplicableReason::kClosedSession));
    EXPECT_EQ(RomBytes(), before);

    ASSERT_THAT(workspace_.Close(id_), IsOk());
    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), AssignmentEdit{"7"}),
                NotApplicable(NotApplicableReason::kClosedSession));
}

TEST_F(NumericEditUseCaseTest, ASessionWithoutADefinitionIsNotApplicable)
{
    Install(std::nullopt);

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), AssignmentEdit{"7"}),
                NotApplicable(NotApplicableReason::kNoDefinition));
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(NumericEditUseCaseTest, UnavailableTargetsAreNotApplicable)
{
    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), AssignmentEdit{"7"}, 1),
                NotApplicable(NotApplicableReason::kUnavailableTarget));

    auto static_axis = GridDefinition();
    static_axis.maps[0].x_axis = {.type = "Static X Axis", .static_data = {"a", "b", "c"}};
    static_axis.maps[0].y_axis = {};
    Install(std::move(static_axis));
    EXPECT_THAT(Edit(NumericTarget::kXAxis, Cell(0, 0), AssignmentEdit{"7"}),
                NotApplicable(NotApplicableReason::kUnavailableTarget));
    EXPECT_THAT(Edit(NumericTarget::kYAxis, Cell(0, 0), AssignmentEdit{"7"}),
                NotApplicable(NotApplicableReason::kUnavailableTarget));

    auto structural = GridDefinition();
    structural.maps[0].address.reset();
    Install(std::move(structural));
    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), AssignmentEdit{"7"}),
                NotApplicable(NotApplicableReason::kUnavailableTarget));

    EXPECT_THAT(RomBytes(), ::testing::Eq(GridRom()));
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(NumericEditUseCaseTest, PastesRowsFromTheSelectionTopLeft)
{
    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 1), PasteEdit{{{"1", "2"}, {"3", "4"}}}), Changed());

    EXPECT_THAT(BodyBytes(), ElementsAre(10, 1, 2, 40, 3, 4));
}

TEST_F(NumericEditUseCaseTest, ClipsALargerSourceToTheRunEdges)
{
    EXPECT_THAT(
        Edit(NumericTarget::kMapBody, Cell(1, 1), PasteEdit{{{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}}}),
        Changed());

    EXPECT_THAT(BodyBytes(), ElementsAre(10, 20, 30, 40, 1, 2));
    const auto rom = RomBytes();
    EXPECT_THAT(std::vector<std::uint8_t>(rom.begin(), rom.begin() + 3), ElementsAre(1, 2, 3));
}

TEST_F(NumericEditUseCaseTest, RaggedRowsPasteWhatEachRowSupplies)
{
    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), PasteEdit{{{"1", "2"}, {"3"}}}), Changed());

    EXPECT_THAT(BodyBytes(), ElementsAre(1, 2, 30, 3, 50, 200));
}

TEST_F(NumericEditUseCaseTest, EverySuppliedCellIsValidatedBeforeClipping)
{
    const auto before = RomBytes();

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), PasteEdit{{{"1", "2", "3", "x"}}}),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("invalid numeric literal")));
    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), PasteEdit{{{"1"}, {"2"}, {"x"}}}),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("invalid numeric literal")));
    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), PasteEdit{{{"1"}, {""}, {"3"}}}),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("empty")));

    EXPECT_EQ(RomBytes(), before);
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(NumericEditUseCaseTest, PasteAcceptsCarriageReturnsAndOnlyDotDecimals)
{
    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(1, 0), PasteEdit{{{"1,5"}}}), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_FALSE(Session().Dirty());

    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), PasteEdit{{{"7\r", "1.6"}}}), Changed());

    EXPECT_THAT(BodyBytes(), ElementsAre(7, 2, 30, 40, 50, 200));
}

TEST_F(NumericEditUseCaseTest, AxesUseTheirOwnGeometry)
{
    EXPECT_THAT(Edit(NumericTarget::kXAxis, Cell(0, 1), PasteEdit{{{"7", "8"}, {"9", "9"}}}), Changed());
    EXPECT_THAT(Edit(NumericTarget::kYAxis, Cell(0, 0), PasteEdit{{{"6", "9"}, {"7", "9"}}}), Changed());

    const auto rom = RomBytes();
    EXPECT_THAT(std::vector<std::uint8_t>(rom.begin(), rom.begin() + 3), ElementsAre(1, 7, 8));
    EXPECT_THAT(std::vector<std::uint8_t>(rom.begin() + 8, rom.begin() + 10), ElementsAre(6, 7));
    EXPECT_THAT(BodyBytes(), ElementsAre(10, 20, 30, 40, 50, 200));
}

TEST_F(NumericEditUseCaseTest, PastingCurrentValuesIsANoOp)
{
    EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0, 0), PasteEdit{{{"10", "20"}}}),
                Unchanged(NoChangeReason::kUnchanged));
    EXPECT_FALSE(Session().Dirty());
}

} // namespace
} // namespace fastecu::calibration
