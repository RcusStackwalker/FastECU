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
        ASSERT_THAT(cfg_.initialize(), IsOk());
        cfg_.file_repository.files["/cal/a.bin"] = std::vector<std::uint8_t>(16, 0xA);
        cfg_.file_repository.files["/cal/b.bin"] = std::vector<std::uint8_t>(16, 0xB);
    }

    config::testing::ConfigSessionFixture cfg_;
    InMemoryAtomicFileWriter writer_;
    definition::DefinitionService definitions_{cfg_.file_system, cfg_.file_repository, writer_};
    testing::FakeDefinitionCatalogs catalogs_;
    RomOpenUseCase opener_{catalogs_, definitions_, cfg_.file_repository, cfg_.file_system, cfg_.events, cfg_.session};
    CalibrationWorkspace workspace_{opener_};
};

TEST_F(CalibrationWorkspaceTest, StartsEmpty)
{
    EXPECT_THAT(workspace_.ids(), IsEmpty());
    EXPECT_EQ(workspace_.find(SessionId{1}), nullptr);
}

TEST_F(CalibrationWorkspaceTest, OpenedSessionsAreFoundInOpenOrder)
{
    const auto a = workspace_.open_file("/cal/a.bin");
    const auto b = workspace_.open_file("/cal/b.bin");

    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(b, IsOk());
    EXPECT_NE(a->id, b->id);
    EXPECT_THAT(workspace_.ids(), ElementsAre(a->id, b->id));
    ASSERT_NE(workspace_.find(a->id), nullptr);
    EXPECT_EQ(workspace_.find(a->id)->source().display_name, "a.bin");
    EXPECT_EQ(workspace_.find(b->id)->rom()[0], 0xB);
}

TEST_F(CalibrationWorkspaceTest, AFailedOpenLeavesTheWorkspaceUnchanged)
{
    const auto a = workspace_.open_file("/cal/a.bin");
    ASSERT_THAT(a, IsOk());

    EXPECT_THAT(workspace_.open_file("/cal/missing.bin"), ::testing::Not(IsOk()));
    EXPECT_THAT(workspace_.adopt_read_image(ReadImage{.filename = "x.bin"}), IsErr(ErrorKind::kInvalidConfig));

    EXPECT_THAT(workspace_.ids(), ElementsAre(a->id));
}

TEST_F(CalibrationWorkspaceTest, AdoptedImagesBecomeSessions)
{
    const auto read = workspace_.adopt_read_image(
        ReadImage{.rom = std::vector<std::uint8_t>(8, 0x5), .filename = "r.bin", .protocol_name = "proto_b"});

    ASSERT_THAT(read, IsOk());
    EXPECT_TRUE(read->vehicle_selected);
    ASSERT_NE(workspace_.find(read->id), nullptr);
    EXPECT_EQ(workspace_.find(read->id)->source().origin, RomOrigin::kEcuRead);
}

TEST_F(CalibrationWorkspaceTest, ClosingKeepsOtherSessionsAndTheirPointers)
{
    const auto a = workspace_.open_file("/cal/a.bin");
    const auto b = workspace_.open_file("/cal/b.bin");
    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(b, IsOk());
    const CalibrationSession *b_before = workspace_.find(b->id);

    ASSERT_THAT(workspace_.close(a->id), IsOk());

    EXPECT_EQ(workspace_.find(a->id), nullptr);
    EXPECT_EQ(workspace_.find(b->id), b_before);
    EXPECT_THAT(workspace_.ids(), ElementsAre(b->id));
}

TEST_F(CalibrationWorkspaceTest, ClosedIdsAreNeverReused)
{
    const auto a = workspace_.open_file("/cal/a.bin");
    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(workspace_.close(a->id), IsOk());

    const auto again = workspace_.open_file("/cal/a.bin");

    ASSERT_THAT(again, IsOk());
    EXPECT_NE(again->id, a->id);
    EXPECT_EQ(workspace_.find(a->id), nullptr);
    EXPECT_THAT(workspace_.close(a->id), IsErr(ErrorKind::kInvalidConfig));
}

TEST_F(CalibrationWorkspaceTest, SessionsAreMutableThroughTheWorkspace)
{
    const auto a = workspace_.open_file("/cal/a.bin");
    ASSERT_THAT(a, IsOk());
    const std::vector<std::uint8_t> patch{0xFF};

    ASSERT_THAT(workspace_.find(a->id)->write_bytes(0, patch), IsOk());

    const CalibrationWorkspace& view = workspace_;
    EXPECT_EQ(view.find(a->id)->rom()[0], 0xFF);
    EXPECT_TRUE(view.find(a->id)->dirty());
}

} // namespace
} // namespace fastecu::calibration
