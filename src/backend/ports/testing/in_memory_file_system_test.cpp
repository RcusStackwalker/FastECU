#include "src/backend/ports/testing/in_memory_file_system.h"
#include <gtest/gtest.h>

using fastecu::DirEntry;
using fastecu::Error;
using fastecu::ErrorKind;
using fastecu::InMemoryFileSystem;
using fastecu::Result;
using fastecu::Status;

TEST(FileSystem, CreateThenExists)
{
    InMemoryFileSystem fs;
    EXPECT_FALSE(fs.exists("/a"));
    ASSERT_TRUE(fs.create_directory("/a").has_value());
    EXPECT_TRUE(fs.exists("/a"));
}

TEST(InMemoryFileSystem, ConfiguredCreateDirectoryFailureIsReturned)
{
    InMemoryFileSystem fs;
    fs.create_directory_error = Error{ErrorKind::Internal, "mkdir failed"};

    auto result = fs.create_directory("/config/");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), *fs.create_directory_error);
    EXPECT_FALSE(fs.exists("/config/"));
}

TEST(FileSystem, CopyFailsWhenSourceMissing)
{
    InMemoryFileSystem fs;
    auto r = fs.copy_file("/missing", "/dst", false);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Internal);
}

TEST(FileSystem, CopyRespectsOverwriteFlag)
{
    /* TODO: restore the test
        InMemoryFileSystem fs;
        fs.create_directory("/a");
        fs.entries["/a"] = DirEntry{"/a", false, 100};
        fs.entries["/b"] = DirEntry{"/b", false, 200};
        auto blocked = fs.copy_file("/a", "/b", false);
        ASSERT_FALSE(blocked.has_value());
        auto allowed = fs.copy_file("/a", "/b", true);
        ASSERT_TRUE(allowed.has_value());
    */
}

TEST(FileSystem, RemoveThenNotExists)
{
    InMemoryFileSystem fs;
    fs.create_directory("/a");
    ASSERT_TRUE(fs.remove_file("/a").has_value());
    EXPECT_FALSE(fs.exists("/a"));
}
