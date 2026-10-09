#include "src/backend/calibration/session/selectable_edit_use_case.h"

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
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::VariantWith;

constexpr std::uint64_t kAddress = 4;

// A selectable map at kAddress whose selections are two bytes wide.
definition::RomDefinition selectable_definition(std::vector<definition::Selection> selections)
{
    definition::RomDefinition rom{.format = definition::DefinitionFormat::EcuFlash};
    rom.scalings.push_back(definition::Scaling{
        .name = "modes", .storage_type = definition::StorageType::Bloblist, .selections = std::move(selections)});
    definition::CalibrationMap map;
    map.name = "Mode";
    map.type = "Selectable";
    map.address = kAddress;
    map.x_size = 1;
    map.y_size = 1;
    map.storage_type = definition::StorageType::Bloblist;
    map.scaling_name = "modes";
    rom.maps.push_back(map);
    return rom;
}

definition::RomDefinition modes_definition()
{
    return selectable_definition({{"off", {0x00, 0x00}}, {"low", {0x0A, 0x0B}}, {"high", {0xFF, 0x10}}});
}

auto changed()
{
    return IsOkAnd(VariantWith<SelectableEditChanged>(::testing::_));
}

auto unchanged()
{
    return IsOkAnd(VariantWith<SelectableEditUnchanged>(::testing::_));
}

auto not_applicable(SelectableNotApplicableReason reason)
{
    return IsOkAnd(VariantWith<SelectableEditNotApplicable>(Field(&SelectableEditNotApplicable::reason, reason)));
}

class SelectableEditUseCaseTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg_.initialize(), IsOk());
        const auto opened = workspace_.adopt_read_image(
            ReadImage{.rom = std::vector<std::uint8_t>(8, 0), .filename = "modes.bin", .protocol_name = "proto_b"});
        ASSERT_THAT(opened, IsOk());
        id_ = opened->id;
        install(modes_definition());
    }

    // Replaces the open session's contents with a clean session.
    void install(std::optional<definition::RomDefinition> def,
                 std::vector<std::uint8_t> rom = std::vector<std::uint8_t>(8, 0x55))
    {
        std::optional<ResolvedDefinition> resolved;
        if (def.has_value())
        {
            resolved = ResolvedDefinition{.definition = std::move(*def)};
        }
        *workspace_.find(id_) = CalibrationSession(id_, SessionContents{.source = {.display_name = "modes.bin"},
                                                                        .rom = std::move(rom),
                                                                        .definition = std::move(resolved),
                                                                        .protocol = {}});
    }

    CalibrationSession& session()
    {
        return *workspace_.find(id_);
    }

    std::vector<std::uint8_t> rom_bytes()
    {
        const auto rom = session().rom();
        return {rom.begin(), rom.end()};
    }

    Result<SelectableEditOutcome> select(const std::string& name, std::size_t map_index = 0)
    {
        return apply_selectable_edit(workspace_, {.session = id_, .map_index = map_index, .selection = name});
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
    EXPECT_THAT(select("low"), changed());

    EXPECT_THAT(rom_bytes(), ElementsAre(0x55, 0x55, 0x55, 0x55, 0x0A, 0x0B, 0x55, 0x55));
    EXPECT_TRUE(session().dirty());
}

TEST_F(SelectableEditUseCaseTest, MismatchedSelectionWidthsAreRejectedWithoutChange)
{
    install(selectable_definition(
        {{"off", {0x00, 0x00}}, {"long", {0xAA, 0xBB, 0xCC}}, {"short", {0x7F}}, {"same", {0x12, 0x34}}}));
    const auto before = rom_bytes();

    EXPECT_THAT(select("long"), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(select("short"), IsErr(ErrorKind::InvalidConfig));

    EXPECT_EQ(rom_bytes(), before);
    EXPECT_FALSE(session().dirty());

    EXPECT_THAT(select("same"), changed());
    EXPECT_THAT(rom_bytes(), ElementsAre(0x55, 0x55, 0x55, 0x55, 0x12, 0x34, 0x55, 0x55));
}

TEST_F(SelectableEditUseCaseTest, AMapWithoutAnAddressWritesAtOffsetZero)
{
    auto no_address = modes_definition();
    no_address.maps[0].address.reset();
    install(std::move(no_address));

    EXPECT_THAT(select("high"), changed());

    EXPECT_THAT(rom_bytes(), ElementsAre(0xFF, 0x10, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55));
}

TEST_F(SelectableEditUseCaseTest, StorageFallsBackToTheScaling)
{
    auto fallback = modes_definition();
    fallback.maps[0].storage_type.reset();
    install(std::move(fallback));

    EXPECT_THAT(select("low"), changed());

    EXPECT_THAT(rom_bytes(), ElementsAre(0x55, 0x55, 0x55, 0x55, 0x0A, 0x0B, 0x55, 0x55));
}

TEST_F(SelectableEditUseCaseTest, WritingTheCurrentBytesIsUnchangedAndLeavesTheSessionClean)
{
    install(modes_definition(), {0, 0, 0, 0, 0x0A, 0x0B, 0, 0});

    EXPECT_THAT(select("low"), unchanged());

    EXPECT_FALSE(session().dirty());
}

TEST_F(SelectableEditUseCaseTest, AWriteOutsideTheImageIsRejectedWithoutChange)
{
    install(modes_definition(), std::vector<std::uint8_t>(5, 0x55));
    const auto before = rom_bytes();

    EXPECT_THAT(select("low"), IsErr(ErrorKind::InvalidConfig));

    EXPECT_EQ(rom_bytes(), before);
    EXPECT_FALSE(session().dirty());
}

TEST_F(SelectableEditUseCaseTest, StaleSessionIdsAreNotApplicable)
{
    const auto before = rom_bytes();
    EXPECT_THAT(apply_selectable_edit(workspace_, {.session = SessionId{999}, .map_index = 0, .selection = "low"}),
                not_applicable(SelectableNotApplicableReason::ClosedSession));
    EXPECT_EQ(rom_bytes(), before);

    ASSERT_THAT(workspace_.close(id_), IsOk());
    EXPECT_THAT(select("low"), not_applicable(SelectableNotApplicableReason::ClosedSession));
}

TEST_F(SelectableEditUseCaseTest, ASessionWithoutADefinitionIsNotApplicable)
{
    install(std::nullopt);

    EXPECT_THAT(select("low"), not_applicable(SelectableNotApplicableReason::NoDefinition));
    EXPECT_FALSE(session().dirty());
}

TEST_F(SelectableEditUseCaseTest, AnUnknownMapIsNotApplicable)
{
    EXPECT_THAT(select("low", 1), not_applicable(SelectableNotApplicableReason::UnknownMap));
    EXPECT_FALSE(session().dirty());
}

TEST_F(SelectableEditUseCaseTest, MapsThatAreNotBlobSelectionsAreNotApplicable)
{
    auto non_blob = modes_definition();
    non_blob.scalings[0].storage_type = definition::StorageType::Uint8;
    non_blob.maps[0].storage_type = definition::StorageType::Uint8;
    install(std::move(non_blob));
    EXPECT_THAT(select("low"), not_applicable(SelectableNotApplicableReason::NotBloblist));

    auto no_selections = modes_definition();
    no_selections.scalings[0].selections.clear();
    install(std::move(no_selections));
    EXPECT_THAT(select("low"), not_applicable(SelectableNotApplicableReason::NotBloblist));

    auto no_scaling = modes_definition();
    no_scaling.maps[0].scaling_name = "missing";
    install(std::move(no_scaling));
    EXPECT_THAT(select("low"), not_applicable(SelectableNotApplicableReason::NotBloblist));
    EXPECT_FALSE(session().dirty());
}

TEST_F(SelectableEditUseCaseTest, AnUnknownSelectionNameChangesNothing)
{
    const auto before = rom_bytes();

    EXPECT_THAT(select("missing"), not_applicable(SelectableNotApplicableReason::UnknownSelection));

    EXPECT_EQ(rom_bytes(), before);
    EXPECT_FALSE(session().dirty());
}

} // namespace
} // namespace fastecu::calibration
