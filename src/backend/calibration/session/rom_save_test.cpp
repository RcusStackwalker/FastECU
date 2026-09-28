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
        ASSERT_THAT(cfg.initialize(), IsOk());
    }
    CalibrationSession session{
        SessionId{1}, SessionContents{
                          .source = {.display_name = "read.bin", .path = "/old/read.bin", .origin = RomOrigin::EcuRead},
                          .rom = {1, 2, 3},
                      }};
    config::testing::ConfigSessionFixture cfg;
    RomSaveUseCase saver{cfg.file_repository, cfg.events};
};

TEST_F(RomSaveTest, SavesEditedBytesAndReopensThem)
{
    const bytes::Bytes patch{9};
    ASSERT_THAT(session.write_bytes(1, patch), IsOk());
    ASSERT_TRUE(session.dirty());
    ASSERT_THAT(saver.save(session, "/cal/saved.bin", session.rom()), IsOk());
    EXPECT_EQ(session.source(), (RomSource{"saved.bin", "/cal/saved.bin", RomOrigin::EcuRead}));
    EXPECT_FALSE(session.dirty());

    InMemoryAtomicFileWriter writer;
    definition::DefinitionService definitions{cfg.file_system, cfg.file_repository, writer};
    testing::FakeDefinitionCatalogs catalogs;
    RomOpenUseCase opener{catalogs, definitions, cfg.file_repository, cfg.file_system, cfg.events, cfg.session};
    const auto reopened = opener.open_file("/cal/saved.bin");
    ASSERT_THAT(reopened, IsOk());
    EXPECT_THAT(reopened->contents.rom, ElementsAre(1, 9, 3));
    EXPECT_EQ(reopened->contents.source.origin, RomOrigin::File);
    ASSERT_THAT(session.write_bytes(0, patch), IsOk());
    EXPECT_TRUE(session.dirty());
}

TEST_F(RomSaveTest, FailurePreservesSourceAndDirtyBytes)
{
    const bytes::Bytes patch{9};
    ASSERT_THAT(session.write_bytes(1, patch), IsOk());
    const RomSource before = session.source();
    cfg.file_repository.write_errors["/cal/fail.bin"] = Error{ErrorKind::Internal, "disk full"};
    cfg.events.logs.clear();
    cfg.events.notices.clear();
    const auto result = saver.save(session, "/cal/fail.bin", session.rom());
    ASSERT_THAT(result, IsErr(ErrorKind::Internal));
    EXPECT_EQ(result.error().detail, "disk full");
    EXPECT_EQ(session.source(), before);
    EXPECT_TRUE(session.dirty());
    EXPECT_THAT(session.rom(), ElementsAre(1, 9, 3));
    EXPECT_THAT(cfg.events.logs,
                ElementsAre(std::pair{LogLevel::Error, std::string{"Unable to open file /cal/fail.bin for writing"}}));
    EXPECT_THAT(cfg.events.notices, ElementsAre("Ecu calibration file: Unable to open file /cal/fail.bin for writing"));
}

TEST_F(RomSaveTest, SavesOperationImageWithoutReplacingSessionBytes)
{
    const bytes::Bytes corrected{4, 5, 6};
    ASSERT_THAT(session.write_bytes(0, bytes::Bytes{9}), IsOk());
    ASSERT_THAT(saver.save(session, "corrected.bin", corrected), IsOk());
    EXPECT_THAT(cfg.file_repository.files.at("corrected.bin"), ElementsAre(4, 5, 6));
    EXPECT_THAT(session.rom(), ElementsAre(9, 2, 3));
    EXPECT_FALSE(session.dirty());
}

TEST_F(RomSaveTest, FailureLeavesAnUneditedSessionClean)
{
    cfg.file_repository.write_errors["failed.bin"] = Error{ErrorKind::Internal, "disk full"};
    const RomSource before = session.source();
    EXPECT_THAT(saver.save(session, "failed.bin", session.rom()), IsErr(ErrorKind::Internal));
    EXPECT_FALSE(session.dirty());
    EXPECT_EQ(session.source(), before);
}

TEST_F(RomSaveTest, SavingToTheCurrentPathPreservesFileOrigin)
{
    CalibrationSession file_session{
        SessionId{2}, SessionContents{
                          .source = {.display_name = "a.bin", .path = "/cal/a.bin", .origin = RomOrigin::File},
                          .rom = {1, 2, 3},
                      }};
    ASSERT_THAT(saver.save(file_session, file_session.source().path, file_session.rom()), IsOk());
    EXPECT_EQ(file_session.source(), (RomSource{"a.bin", "/cal/a.bin", RomOrigin::File}));
}

TEST_F(RomSaveTest, SavedPathWithoutBasenameUsesDefaultName)
{
    session.mark_saved("/cal/");
    EXPECT_EQ(session.source(), (RomSource{"default.bin", "/cal/", RomOrigin::EcuRead}));
    session.mark_saved("");
    EXPECT_EQ(session.source().display_name, "default.bin");
    session.mark_saved("plain.bin");
    EXPECT_EQ(session.source().display_name, "plain.bin");
}
} // namespace
} // namespace fastecu::calibration
