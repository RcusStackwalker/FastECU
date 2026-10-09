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
        ASSERT_THAT(cfg_.Initialize(), IsOk());
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
    EXPECT_THAT(workspace_.Ids(), IsEmpty());
    EXPECT_EQ(workspace_.Find(SessionId{1}), nullptr);
}

TEST_F(CalibrationWorkspaceTest, OpenedSessionsAreFoundInOpenOrder)
{
    const auto a = workspace_.OpenFile("/cal/a.bin");
    const auto b = workspace_.OpenFile("/cal/b.bin");

    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(b, IsOk());
    EXPECT_NE(a->id, b->id);
    EXPECT_THAT(workspace_.Ids(), ElementsAre(a->id, b->id));
    ASSERT_NE(workspace_.Find(a->id), nullptr);
    EXPECT_EQ(workspace_.Find(a->id)->Source().display_name, "a.bin");
    EXPECT_EQ(workspace_.Find(b->id)->Rom()[0], 0xB);
}

TEST_F(CalibrationWorkspaceTest, AFailedOpenLeavesTheWorkspaceUnchanged)
{
    const auto a = workspace_.OpenFile("/cal/a.bin");
    ASSERT_THAT(a, IsOk());

    EXPECT_THAT(workspace_.OpenFile("/cal/missing.bin"), ::testing::Not(IsOk()));
    EXPECT_THAT(workspace_.AdoptReadImage(ReadImage{.filename = "x.bin"}), IsErr(ErrorKind::kInvalidConfig));

    EXPECT_THAT(workspace_.Ids(), ElementsAre(a->id));
}

TEST_F(CalibrationWorkspaceTest, AdoptedImagesBecomeSessions)
{
    const auto read = workspace_.AdoptReadImage(
        ReadImage{.rom = std::vector<std::uint8_t>(8, 0x5), .filename = "r.bin", .protocol_name = "proto_b"});

    ASSERT_THAT(read, IsOk());
    EXPECT_TRUE(read->vehicle_selected);
    ASSERT_NE(workspace_.Find(read->id), nullptr);
    EXPECT_EQ(workspace_.Find(read->id)->Source().origin, RomOrigin::kEcuRead);
}

TEST_F(CalibrationWorkspaceTest, ClosingKeepsOtherSessionsAndTheirPointers)
{
    const auto a = workspace_.OpenFile("/cal/a.bin");
    const auto b = workspace_.OpenFile("/cal/b.bin");
    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(b, IsOk());
    const CalibrationSession *b_before = workspace_.Find(b->id);

    ASSERT_THAT(workspace_.Close(a->id), IsOk());

    EXPECT_EQ(workspace_.Find(a->id), nullptr);
    EXPECT_EQ(workspace_.Find(b->id), b_before);
    EXPECT_THAT(workspace_.Ids(), ElementsAre(b->id));
}

TEST_F(CalibrationWorkspaceTest, ClosedIdsAreNeverReused)
{
    const auto a = workspace_.OpenFile("/cal/a.bin");
    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(workspace_.Close(a->id), IsOk());

    const auto again = workspace_.OpenFile("/cal/a.bin");

    ASSERT_THAT(again, IsOk());
    EXPECT_NE(again->id, a->id);
    EXPECT_EQ(workspace_.Find(a->id), nullptr);
    EXPECT_THAT(workspace_.Close(a->id), IsErr(ErrorKind::kInvalidConfig));
}

TEST_F(CalibrationWorkspaceTest, SessionsAreMutableThroughTheWorkspace)
{
    const auto a = workspace_.OpenFile("/cal/a.bin");
    ASSERT_THAT(a, IsOk());
    const std::vector<std::uint8_t> patch{0xFF};

    ASSERT_THAT(workspace_.Find(a->id)->WriteBytes(0, patch), IsOk());

    const CalibrationWorkspace& view = workspace_;
    EXPECT_EQ(view.Find(a->id)->Rom()[0], 0xFF);
    EXPECT_TRUE(view.Find(a->id)->Dirty());
}

} // namespace
} // namespace fastecu::calibration
