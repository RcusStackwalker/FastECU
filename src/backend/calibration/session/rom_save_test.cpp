#include "src/backend/calibration/session/rom_save.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/calibration/session/rom_open.h"
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

class RomSaveTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg_.initialize(), IsOk());
    }
    CalibrationSession session_{
        SessionId{1},
        SessionContents{
            .source = {.display_name = "read.bin", .path = "/old/read.bin", .origin = RomOrigin::kEcuRead},
            .rom = {1, 2, 3},
        }};
    config::testing::ConfigSessionFixture cfg_;
    RomSaveUseCase saver_{cfg_.file_repository, cfg_.events};
};

TEST_F(RomSaveTest, SavesEditedBytesAndReopensThem)
{
    const bytes::Bytes patch{9};
    ASSERT_THAT(session_.write_bytes(1, patch), IsOk());
    ASSERT_TRUE(session_.dirty());
    ASSERT_THAT(saver_.save(session_, "/cal/saved.bin", session_.rom()), IsOk());
    EXPECT_EQ(session_.source(), (RomSource{"saved.bin", "/cal/saved.bin", RomOrigin::kEcuRead}));
    EXPECT_FALSE(session_.dirty());

    InMemoryAtomicFileWriter writer;
    definition::DefinitionService definitions{cfg_.file_system, cfg_.file_repository, writer};
    testing::FakeDefinitionCatalogs catalogs;
    RomOpenUseCase opener{catalogs, definitions, cfg_.file_repository, cfg_.file_system, cfg_.events, cfg_.session};
    const auto reopened = opener.open_file("/cal/saved.bin");
    ASSERT_THAT(reopened, IsOk());
    EXPECT_THAT(reopened->contents.rom, ElementsAre(1, 9, 3));
    EXPECT_EQ(reopened->contents.source.origin, RomOrigin::kFile);
    ASSERT_THAT(session_.write_bytes(0, patch), IsOk());
    EXPECT_TRUE(session_.dirty());
}

TEST_F(RomSaveTest, FailurePreservesSourceAndDirtyBytes)
{
    const bytes::Bytes patch{9};
    ASSERT_THAT(session_.write_bytes(1, patch), IsOk());
    const RomSource before = session_.source();
    cfg_.file_repository.write_errors["/cal/fail.bin"] = Error{ErrorKind::kInternal, "disk full"};
    cfg_.events.logs.clear();
    cfg_.events.notices.clear();
    const auto result = saver_.save(session_, "/cal/fail.bin", session_.rom());
    ASSERT_THAT(result, IsErr(ErrorKind::kInternal));
    EXPECT_EQ(result.error().detail, "disk full");
    EXPECT_EQ(session_.source(), before);
    EXPECT_TRUE(session_.dirty());
    EXPECT_THAT(session_.rom(), ElementsAre(1, 9, 3));
    EXPECT_THAT(cfg_.events.logs,
                ElementsAre(std::pair{LogLevel::kError, std::string{"Unable to open file /cal/fail.bin for writing"}}));
    EXPECT_THAT(cfg_.events.notices,
                ElementsAre("Ecu calibration file: Unable to open file /cal/fail.bin for writing"));
}

TEST_F(RomSaveTest, SavesOperationImageWithoutReplacingSessionBytes)
{
    const bytes::Bytes corrected{4, 5, 6};
    ASSERT_THAT(session_.write_bytes(0, bytes::Bytes{9}), IsOk());
    ASSERT_THAT(saver_.save(session_, "corrected.bin", corrected), IsOk());
    EXPECT_THAT(cfg_.file_repository.files.at("corrected.bin"), ElementsAre(4, 5, 6));
    EXPECT_THAT(session_.rom(), ElementsAre(9, 2, 3));
    EXPECT_FALSE(session_.dirty());
}

TEST_F(RomSaveTest, FailureLeavesAnUneditedSessionClean)
{
    cfg_.file_repository.write_errors["failed.bin"] = Error{ErrorKind::kInternal, "disk full"};
    const RomSource before = session_.source();
    EXPECT_THAT(saver_.save(session_, "failed.bin", session_.rom()), IsErr(ErrorKind::kInternal));
    EXPECT_FALSE(session_.dirty());
    EXPECT_EQ(session_.source(), before);
}

TEST_F(RomSaveTest, SavingToTheCurrentPathPreservesFileOrigin)
{
    CalibrationSession file_session{
        SessionId{2}, SessionContents{
                          .source = {.display_name = "a.bin", .path = "/cal/a.bin", .origin = RomOrigin::kFile},
                          .rom = {1, 2, 3},
                      }};
    ASSERT_THAT(saver_.save(file_session, file_session.source().path, file_session.rom()), IsOk());
    EXPECT_EQ(file_session.source(), (RomSource{"a.bin", "/cal/a.bin", RomOrigin::kFile}));
}

TEST_F(RomSaveTest, SavedPathWithoutBasenameUsesDefaultName)
{
    session_.mark_saved("/cal/");
    EXPECT_EQ(session_.source(), (RomSource{"default.bin", "/cal/", RomOrigin::kEcuRead}));
    session_.mark_saved("");
    EXPECT_EQ(session_.source().display_name, "default.bin");
    session_.mark_saved("plain.bin");
    EXPECT_EQ(session_.source().display_name, "plain.bin");
}
} // namespace
} // namespace fastecu::calibration
