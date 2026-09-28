#include "src/backend/calibration/session/calibration_workspace.h"

#include <cstdint>
#include <string>
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
using ::testing::ElementsAre;
using ::testing::IsEmpty;

class CalibrationWorkspaceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg.initialize(), IsOk());
        cfg.file_repository.files["/cal/a.bin"] = std::vector<std::uint8_t>(16, 0xA);
        cfg.file_repository.files["/cal/b.bin"] = std::vector<std::uint8_t>(16, 0xB);
    }

    config::testing::ConfigSessionFixture cfg;
    InMemoryAtomicFileWriter writer;
    definition::DefinitionService definitions{cfg.file_system, cfg.file_repository, writer};
    testing::FakeDefinitionCatalogs catalogs;
    RomOpenUseCase opener{catalogs, definitions, cfg.file_repository, cfg.file_system, cfg.events, cfg.session};
    CalibrationWorkspace workspace{opener};
};

TEST_F(CalibrationWorkspaceTest, StartsEmpty)
{
    EXPECT_THAT(workspace.ids(), IsEmpty());
    EXPECT_EQ(workspace.find(SessionId{1}), nullptr);
}

TEST_F(CalibrationWorkspaceTest, OpenedSessionsAreFoundInOpenOrder)
{
    const auto a = workspace.open_file("/cal/a.bin");
    const auto b = workspace.open_file("/cal/b.bin");

    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(b, IsOk());
    EXPECT_NE(a->id, b->id);
    EXPECT_THAT(workspace.ids(), ElementsAre(a->id, b->id));
    ASSERT_NE(workspace.find(a->id), nullptr);
    EXPECT_EQ(workspace.find(a->id)->source().display_name, "a.bin");
    EXPECT_EQ(workspace.find(b->id)->rom()[0], 0xB);
}

TEST_F(CalibrationWorkspaceTest, AFailedOpenLeavesTheWorkspaceUnchanged)
{
    const auto a = workspace.open_file("/cal/a.bin");
    ASSERT_THAT(a, IsOk());

    EXPECT_THAT(workspace.open_file("/cal/missing.bin"), ::testing::Not(IsOk()));
    EXPECT_THAT(workspace.adopt_read_image(ReadImage{.filename = "x.bin"}), IsErr(ErrorKind::InvalidConfig));

    EXPECT_THAT(workspace.ids(), ElementsAre(a->id));
}

TEST_F(CalibrationWorkspaceTest, AdoptedImagesBecomeSessions)
{
    const auto read = workspace.adopt_read_image(
        ReadImage{.rom = std::vector<std::uint8_t>(8, 0x5), .filename = "r.bin", .protocol_name = "proto_b"});

    ASSERT_THAT(read, IsOk());
    EXPECT_TRUE(read->vehicle_selected);
    ASSERT_NE(workspace.find(read->id), nullptr);
    EXPECT_EQ(workspace.find(read->id)->source().origin, RomOrigin::EcuRead);
}

TEST_F(CalibrationWorkspaceTest, ClosingKeepsOtherSessionsAndTheirPointers)
{
    const auto a = workspace.open_file("/cal/a.bin");
    const auto b = workspace.open_file("/cal/b.bin");
    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(b, IsOk());
    const CalibrationSession *b_before = workspace.find(b->id);

    ASSERT_THAT(workspace.close(a->id), IsOk());

    EXPECT_EQ(workspace.find(a->id), nullptr);
    EXPECT_EQ(workspace.find(b->id), b_before);
    EXPECT_THAT(workspace.ids(), ElementsAre(b->id));
}

TEST_F(CalibrationWorkspaceTest, ClosedIdsAreNeverReused)
{
    const auto a = workspace.open_file("/cal/a.bin");
    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(workspace.close(a->id), IsOk());

    const auto again = workspace.open_file("/cal/a.bin");

    ASSERT_THAT(again, IsOk());
    EXPECT_NE(again->id, a->id);
    EXPECT_EQ(workspace.find(a->id), nullptr);
    EXPECT_THAT(workspace.close(a->id), IsErr(ErrorKind::InvalidConfig));
}

TEST_F(CalibrationWorkspaceTest, SessionsAreMutableThroughTheWorkspace)
{
    const auto a = workspace.open_file("/cal/a.bin");
    ASSERT_THAT(a, IsOk());
    const std::vector<std::uint8_t> patch{0xFF};

    ASSERT_THAT(workspace.find(a->id)->write_bytes(0, patch), IsOk());

    const CalibrationWorkspace& view = workspace;
    EXPECT_EQ(view.find(a->id)->rom()[0], 0xFF);
    EXPECT_TRUE(view.find(a->id)->dirty());
}

} // namespace
} // namespace fastecu::calibration
