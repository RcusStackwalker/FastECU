#include "src/backend/calibration/session/selectable_edit_use_case.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/memory/testing/memory_views.h"
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
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::VariantWith;

constexpr std::uint64_t kAddress = 4;

// A selectable map at kAddress whose selections are two bytes wide.
definition::RomDefinition SelectableDefinition(std::vector<definition::Selection> selections)
{
    definition::RomDefinition rom{.format = definition::DefinitionFormat::kEcuFlash};
    rom.scalings.push_back(definition::Scaling{
        .name = "modes", .storage_type = definition::StorageType::kBloblist, .selections = std::move(selections)});
    definition::CalibrationMap map;
    map.name = "Mode";
    map.type = "Selectable";
    map.address = memory::DefinitionAddress{kAddress};
    map.x_size = 1;
    map.y_size = 1;
    map.storage_type = definition::StorageType::kBloblist;
    map.scaling_name = "modes";
    rom.maps.push_back(map);
    return rom;
}

definition::RomDefinition ModesDefinition()
{
    return SelectableDefinition({{"off", {0x00, 0x00}}, {"low", {0x0A, 0x0B}}, {"high", {0xFF, 0x10}}});
}

auto Changed()
{
    return IsOkAnd(VariantWith<SelectableEditChanged>(::testing::_));
}

auto Unchanged()
{
    return IsOkAnd(VariantWith<SelectableEditUnchanged>(::testing::_));
}

auto NotApplicable(SelectableNotApplicableReason reason)
{
    return IsOkAnd(VariantWith<SelectableEditNotApplicable>(Field(&SelectableEditNotApplicable::reason, reason)));
}

class SelectableEditUseCaseTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg_.Initialize(), IsOk());
        const auto opened = workspace_.AdoptReadImage(
            ReadImage{.rom = std::vector<std::uint8_t>(8, 0), .filename = "modes.bin", .protocol_name = "proto_b"});
        ASSERT_THAT(opened, IsOk());
        id_ = opened->id;
        Install(ModesDefinition());
    }

    // Replaces the open session's contents with a clean session.
    void Install(std::optional<definition::RomDefinition> def,
                 std::vector<std::uint8_t> rom = std::vector<std::uint8_t>(8, 0x55))
    {
        std::optional<ResolvedDefinition> resolved;
        if (def.has_value())
        {
            resolved = ResolvedDefinition{.definition = std::move(*def)};
        }
        *workspace_.Find(id_) =
            CalibrationSession(id_, SessionContents{.source = {.display_name = "modes.bin"},
                                                    .image = memory::testing::IdentityImage(std::move(rom)),
                                                    .definition = std::move(resolved),
                                                    .protocol = {}});
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

    Result<SelectableEditOutcome> Select(const std::string& name, std::size_t map_index = 0)
    {
        return ApplySelectableEdit(workspace_, {.session = id_, .map_index = map_index, .selection = name});
    }

    config::testing::ConfigSessionFixture cfg_;
    InMemoryAtomicFileWriter writer_;
    definition::DefinitionService definitions_{cfg_.file_system, cfg_.file_repository, writer_};
    testing::FakeDefinitionCatalogs catalogs_;
    RomOpenUseCase opener_{catalogs_, definitions_, cfg_.file_repository, cfg_.file_system, cfg_.events, cfg_.session};
    CalibrationWorkspace workspace_{opener_};
    SessionId id_{};
};

TEST_F(SelectableEditUseCaseTest, WritesTheNamedSelectionAtTheMapAddress)
{
    EXPECT_THAT(Select("low"), Changed());

    EXPECT_THAT(RomBytes(), ElementsAre(0x55, 0x55, 0x55, 0x55, 0x0A, 0x0B, 0x55, 0x55));
    EXPECT_TRUE(Session().Dirty());
}

TEST_F(SelectableEditUseCaseTest, MismatchedSelectionWidthsAreRejectedWithoutChange)
{
    Install(SelectableDefinition(
        {{"off", {0x00, 0x00}}, {"long", {0xAA, 0xBB, 0xCC}}, {"short", {0x7F}}, {"same", {0x12, 0x34}}}));
    const auto before = RomBytes();

    EXPECT_THAT(Select("long"), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(Select("short"), IsErr(ErrorKind::kInvalidConfig));

    EXPECT_EQ(RomBytes(), before);
    EXPECT_FALSE(Session().Dirty());

    EXPECT_THAT(Select("same"), Changed());
    EXPECT_THAT(RomBytes(), ElementsAre(0x55, 0x55, 0x55, 0x55, 0x12, 0x34, 0x55, 0x55));
}

TEST_F(SelectableEditUseCaseTest, AMapWithoutAnAddressWritesAtOffsetZero)
{
    auto no_address = ModesDefinition();
    no_address.maps[0].address.reset();
    Install(std::move(no_address));

    EXPECT_THAT(Select("high"), Changed());

    EXPECT_THAT(RomBytes(), ElementsAre(0xFF, 0x10, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55));
}

TEST_F(SelectableEditUseCaseTest, StorageFallsBackToTheScaling)
{
    auto fallback = ModesDefinition();
    fallback.maps[0].storage_type.reset();
    Install(std::move(fallback));

    EXPECT_THAT(Select("low"), Changed());

    EXPECT_THAT(RomBytes(), ElementsAre(0x55, 0x55, 0x55, 0x55, 0x0A, 0x0B, 0x55, 0x55));
}

TEST_F(SelectableEditUseCaseTest, WritingTheCurrentBytesIsUnchangedAndLeavesTheSessionClean)
{
    Install(ModesDefinition(), {0, 0, 0, 0, 0x0A, 0x0B, 0, 0});

    EXPECT_THAT(Select("low"), Unchanged());

    EXPECT_FALSE(Session().Dirty());
}

TEST_F(SelectableEditUseCaseTest, AWriteOutsideTheImageIsRejectedWithoutChange)
{
    Install(ModesDefinition(), std::vector<std::uint8_t>(5, 0x55));
    const auto before = RomBytes();

    EXPECT_THAT(Select("low"), IsErr(ErrorKind::kInvalidConfig));

    EXPECT_EQ(RomBytes(), before);
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(SelectableEditUseCaseTest, StaleSessionIdsAreNotApplicable)
{
    const auto before = RomBytes();
    EXPECT_THAT(ApplySelectableEdit(workspace_, {.session = SessionId{999}, .map_index = 0, .selection = "low"}),
                NotApplicable(SelectableNotApplicableReason::kClosedSession));
    EXPECT_EQ(RomBytes(), before);

    ASSERT_THAT(workspace_.Close(id_), IsOk());
    EXPECT_THAT(Select("low"), NotApplicable(SelectableNotApplicableReason::kClosedSession));
}

TEST_F(SelectableEditUseCaseTest, ASessionWithoutADefinitionIsNotApplicable)
{
    Install(std::nullopt);

    EXPECT_THAT(Select("low"), NotApplicable(SelectableNotApplicableReason::kNoDefinition));
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(SelectableEditUseCaseTest, AnUnknownMapIsNotApplicable)
{
    EXPECT_THAT(Select("low", 1), NotApplicable(SelectableNotApplicableReason::kUnknownMap));
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(SelectableEditUseCaseTest, MapsThatAreNotBlobSelectionsAreNotApplicable)
{
    auto non_blob = ModesDefinition();
    non_blob.scalings[0].storage_type = definition::StorageType::kUint8;
    non_blob.maps[0].storage_type = definition::StorageType::kUint8;
    Install(std::move(non_blob));
    EXPECT_THAT(Select("low"), NotApplicable(SelectableNotApplicableReason::kNotBloblist));

    auto no_selections = ModesDefinition();
    no_selections.scalings[0].selections.clear();
    Install(std::move(no_selections));
    EXPECT_THAT(Select("low"), NotApplicable(SelectableNotApplicableReason::kNotBloblist));

    auto no_scaling = ModesDefinition();
    no_scaling.maps[0].scaling_name = "missing";
    Install(std::move(no_scaling));
    EXPECT_THAT(Select("low"), NotApplicable(SelectableNotApplicableReason::kNotBloblist));
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(SelectableEditUseCaseTest, AnUnknownSelectionNameChangesNothing)
{
    const auto before = RomBytes();

    EXPECT_THAT(Select("missing"), NotApplicable(SelectableNotApplicableReason::kUnknownSelection));

    EXPECT_EQ(RomBytes(), before);
    EXPECT_FALSE(Session().Dirty());
}

} // namespace
} // namespace fastecu::calibration
