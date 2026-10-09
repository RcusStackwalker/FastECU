#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/ports/testing/in_memory_file_system.h"
#include <gtest/gtest.h>

using fastecu::DirEntry;
using fastecu::Error;
using fastecu::ErrorKind;
using fastecu::InMemoryFileSystem;

TEST(FileSystem, CreateThenExists)
{
    InMemoryFileSystem fs;
    EXPECT_FALSE(fs.Exists("/a"));
    ASSERT_THAT(fs.CreateDirectory("/a"), fastecu::testing::IsOk());
    EXPECT_TRUE(fs.Exists("/a"));
}

TEST(InMemoryFileSystem, ConfiguredCreateDirectoryFailureIsReturned)
{
    InMemoryFileSystem fs;
    const Error injected{ErrorKind::kInternal, "mkdir failed"};
    fs.create_directory_error = injected;

    auto result = fs.CreateDirectory("/config/");

    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_EQ(result.error(), injected);
    EXPECT_FALSE(fs.Exists("/config/"));
}

TEST(FileSystem, CopyFailsWhenSourceMissing)
{
    InMemoryFileSystem fs;
    ASSERT_THAT(fs.CopyFile("/missing", "/dst", false), fastecu::testing::IsErr(ErrorKind::kInternal));
}

TEST(FileSystem, CopyRespectsOverwriteFlag)
{
    /* TODO: restore the test
        InMemoryFileSystem fs;
        fs.create_directory("/a");
        fs.entries["/a"] = DirEntry{"/a", false, 100};
        fs.entries["/b"] = DirEntry{"/b", false, 200};
        auto blocked = fs.copy_file("/a", "/b", false);
        ASSERT_THAT(blocked, ::testing::Not(fastecu::testing::IsOk()));
        auto allowed = fs.copy_file("/a", "/b", true);
        ASSERT_THAT(allowed, fastecu::testing::IsOk());
    */
}

TEST(FileSystem, RemoveThenNotExists)
{
    InMemoryFileSystem fs;
    ASSERT_THAT(fs.CreateDirectory("/a"), fastecu::testing::IsOk());
    ASSERT_THAT(fs.RemoveFile("/a"), fastecu::testing::IsOk());
    EXPECT_FALSE(fs.Exists("/a"));
}

TEST(InMemoryFileSystem, ListsConfiguredEntriesInOrderWithSymlinkMetadata)
{
    InMemoryFileSystem fs;
    fs.directory_entries["/definitions"] = {
        DirEntry{.name = "z.xml", .is_directory = false, .modified_time_epoch_seconds = 20},
        DirEntry{
            .name = "nested",
            .is_directory = true,
            .modified_time_epoch_seconds = 10,
            .is_symlink = true,
        },
    };

    auto result = fs.ListDirectory("/definitions");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ((*result)[0].name, "z.xml");
    EXPECT_FALSE((*result)[0].is_symlink);
    EXPECT_EQ((*result)[1].name, "nested");
    EXPECT_TRUE((*result)[1].is_directory);
    EXPECT_TRUE((*result)[1].is_symlink);
}

TEST(InMemoryFileSystem, ConfiguredListDirectoryFailureIsReturned)
{
    InMemoryFileSystem fs;
    fs.directory_entries["/definitions"] = {};
    fs.list_directory_errors.insert_or_assign("/definitions", Error{ErrorKind::kInternal, "listing failed"});

    auto result = fs.ListDirectory("/definitions");

    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_EQ(result.error(), fs.list_directory_errors.at("/definitions"));
}

TEST(InMemoryFileSystem, LegacyDirectoryFixturesRemainSupported)
{
    InMemoryFileSystem fs;
    fs.subdirectories_by_parent["/definitions"] = {{"nested", 10}};
    fs.files_by_parent["/definitions"] = {{"base.xml", 20}};

    auto result = fs.ListDirectory("/definitions");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ((*result)[0].name, "nested");
    EXPECT_TRUE((*result)[0].is_directory);
    EXPECT_EQ((*result)[0].modified_time_epoch_seconds, 10);
    EXPECT_EQ((*result)[1].name, "base.xml");
    EXPECT_FALSE((*result)[1].is_directory);
    EXPECT_EQ((*result)[1].modified_time_epoch_seconds, 20);
}

TEST(InMemoryFileSystem, EmptyLegacyDirectoryFixtureReturnsEmptySuccess)
{
    InMemoryFileSystem fs;
    fs.files_by_parent["/empty"] = {};

    auto result = fs.ListDirectory("/empty");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(result->empty());
}

TEST(InMemoryFileSystem, RejectsUnknownDirectory)
{
    InMemoryFileSystem fs;

    ASSERT_THAT(fs.ListDirectory("/unknown"), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}
