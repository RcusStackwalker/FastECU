#include "src/backend/ports/file_system.h"
#include <gtest/gtest.h>
#include <map>

using fastecu::DirEntry;
using fastecu::ErrorKind;
using fastecu::IFileSystem;
using fastecu::Result;
using fastecu::Status;

namespace
{
class InMemoryFileSystem : public IFileSystem
{
  public:
    bool exists(std::string_view path) override
    {
        return entries.count(std::string(path)) > 0;
    }
    Status create_directory(std::string_view path) override
    {
        entries[std::string(path)] = DirEntry{std::string(path), true, 0};
        return {};
    }
    Status copy_file(std::string_view src, std::string_view dst, bool overwrite) override
    {
        if (!entries.count(std::string(src)))
            return fastecu::fail(ErrorKind::Internal, "source missing");
        if (!overwrite && entries.count(std::string(dst)))
            return fastecu::fail(ErrorKind::Internal, "destination exists");
        entries[std::string(dst)] = entries[std::string(src)];
        return {};
    }
    Status remove_file(std::string_view path) override
    {
        entries.erase(std::string(path));
        return {};
    }
    Result<std::vector<DirEntry>> list_directory(std::string_view path) override
    {
        (void)path;
        std::vector<DirEntry> out;
        for (auto& [k, v] : entries)
            out.push_back(v);
        return out;
    }

    std::map<std::string, DirEntry> entries;
};
} // namespace

TEST(FileSystem, CreateThenExists)
{
    InMemoryFileSystem fs;
    EXPECT_FALSE(fs.exists("/a"));
    ASSERT_TRUE(fs.create_directory("/a").has_value());
    EXPECT_TRUE(fs.exists("/a"));
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
    InMemoryFileSystem fs;
    fs.create_directory("/a");
    fs.entries["/a"] = DirEntry{"/a", false, 100};
    fs.entries["/b"] = DirEntry{"/b", false, 200};
    auto blocked = fs.copy_file("/a", "/b", false);
    ASSERT_FALSE(blocked.has_value());
    auto allowed = fs.copy_file("/a", "/b", true);
    ASSERT_TRUE(allowed.has_value());
}

TEST(FileSystem, RemoveThenNotExists)
{
    InMemoryFileSystem fs;
    fs.create_directory("/a");
    ASSERT_TRUE(fs.remove_file("/a").has_value());
    EXPECT_FALSE(fs.exists("/a"));
}
