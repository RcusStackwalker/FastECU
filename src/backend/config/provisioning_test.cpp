#include "src/backend/config/provisioning.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <map>

using fastecu::DirEntry;
using fastecu::ErrorKind;
using fastecu::IEventSink;
using fastecu::IFileSystem;
using fastecu::IResourceBundle;
using fastecu::LogLevel;
using fastecu::Result;
using fastecu::Status;
using fastecu::config::ConfigPaths;
using fastecu::config::provision_config_directories;

namespace
{
class FakeFileSystem : public IFileSystem
{
  public:
    bool exists(std::string_view path) override
    {
        return directories.count(std::string(path)) || files.count(std::string(path));
    }
    Status create_directory(std::string_view path) override
    {
        directories.insert(std::string(path));
        return {};
    }
    Status copy_file(std::string_view src, std::string_view dst, bool overwrite) override
    {
        if (!files.count(std::string(src)))
            return fastecu::fail(ErrorKind::Internal, "source missing");
        if (!overwrite && files.count(std::string(dst)))
            return fastecu::fail(ErrorKind::Internal, "destination exists");
        files[std::string(dst)] = files[std::string(src)];
        copy_calls.push_back({std::string(src), std::string(dst)});
        return {};
    }
    Status remove_file(std::string_view path) override
    {
        files.erase(std::string(path));
        removed.push_back(std::string(path));
        return {};
    }
    Result<std::vector<DirEntry>> list_directory(std::string_view path) override
    {
        std::vector<DirEntry> out;
        for (auto& [name, mtime] : subdirectories_by_parent[std::string(path)])
            out.push_back(DirEntry{name, true, mtime});
        for (auto& [name, mtime] : files_by_parent[std::string(path)])
            out.push_back(DirEntry{name, false, mtime});
        return out;
    }

    std::set<std::string> directories;
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::map<std::string, std::vector<std::pair<std::string, std::int64_t>>> subdirectories_by_parent;
    std::map<std::string, std::vector<std::pair<std::string, std::int64_t>>> files_by_parent;
    std::vector<std::pair<std::string, std::string>> copy_calls;
    std::vector<std::string> removed;
};

class FakeResourceBundle : public IResourceBundle
{
  public:
    Result<std::vector<std::string>> list(std::string_view bundle_id) override
    {
        std::vector<std::string> names;
        for (auto& [name, bytes] : bundles[std::string(bundle_id)])
            names.push_back(name);
        return names;
    }
    Result<std::vector<std::uint8_t>> read(std::string_view bundle_id, std::string_view name) override
    {
        return bundles[std::string(bundle_id)][std::string(name)];
    }

    std::map<std::string, std::map<std::string, std::vector<std::uint8_t>>> bundles;
};

class RecordingEventSink : public IEventSink
{
  public:
    void log(LogLevel, std::string_view) override
    {
    }
    void progress(int, int) override
    {
    }
    void notice(std::string_view) override
    {
    }
};

ConfigPaths test_paths()
{
    ConfigPaths p;
    p.base_config_directory = "/base";
    p.version_config_directory = "/base/1.0/";
    p.calibration_files_directory = "/base/1.0/calibrations/";
    p.config_files_directory = "/base/1.0/config/";
    p.definition_files_directory = "/base/1.0/definitions/";
    p.kernel_files_directory = "/base/1.0/kernels/";
    p.datalog_files_directory = "/base/1.0/datalogs/";
    p.syslog_files_directory = "/base/1.0/syslogs/";
    return p;
}
} // namespace

TEST(ProvisionConfigDirectories, CreatesEveryDirectoryOnFirstRun)
{
    FakeFileSystem fs;
    FakeResourceBundle bundle;
    RecordingEventSink events;
    ConfigPaths paths = test_paths();

    ASSERT_TRUE(provision_config_directories(paths, fs, bundle, events).has_value());

    EXPECT_TRUE(fs.exists(paths.base_config_directory));
    EXPECT_TRUE(fs.exists(paths.calibration_files_directory));
    EXPECT_TRUE(fs.exists(paths.config_files_directory));
    EXPECT_TRUE(fs.exists(paths.definition_files_directory));
    EXPECT_TRUE(fs.exists(paths.kernel_files_directory));
    EXPECT_TRUE(fs.exists(paths.datalog_files_directory));
    EXPECT_TRUE(fs.exists(paths.syslog_files_directory));
}

TEST(ProvisionConfigDirectories, IdempotentOnSecondRun)
{
    FakeFileSystem fs;
    FakeResourceBundle bundle;
    RecordingEventSink events;
    ConfigPaths paths = test_paths();

    ASSERT_TRUE(provision_config_directories(paths, fs, bundle, events).has_value());
    auto directories_after_first = fs.directories;
    ASSERT_TRUE(provision_config_directories(paths, fs, bundle, events).has_value());
    EXPECT_EQ(fs.directories, directories_after_first);
}

TEST(ProvisionConfigDirectories, CopiesBundledResourceFilesNotAlreadyPresent)
{
    FakeFileSystem fs;
    FakeResourceBundle bundle;
    RecordingEventSink events;
    ConfigPaths paths = test_paths();
    bundle.bundles["config"]["fastecu.cfg"] = {1};
    bundle.bundles["kernels"]["k1.bin"] = {2};
    // The bundle's contents are compiled-in resources (Qt ":/..." paths in
    // production) that already exist as QFile-readable files before
    // provisioning runs -- list() enumerates their names, copy_file reads
    // their bytes directly. Model that pre-existing-ness here so the fake
    // mirrors production instead of requiring a nonexistent
    // IFileSystem::write_bytes to bridge the two ports.
    fs.files["config/fastecu.cfg"] = {1};
    fs.files["kernels/k1.bin"] = {2};

    ASSERT_TRUE(provision_config_directories(paths, fs, bundle, events).has_value());

    EXPECT_TRUE(fs.exists(paths.config_files_directory + "fastecu.cfg"));
    EXPECT_TRUE(fs.exists(paths.kernel_files_directory + "k1.bin"));
}

TEST(ProvisionConfigDirectories, DoesNotOverwriteAnExistingUserFile)
{
    FakeFileSystem fs;
    FakeResourceBundle bundle;
    RecordingEventSink events;
    ConfigPaths paths = test_paths();
    bundle.bundles["config"]["fastecu.cfg"] = {9, 9, 9};
    fs.create_directory(paths.config_files_directory);
    fs.files[paths.config_files_directory + "fastecu.cfg"] = {1, 2, 3}; // user's own copy

    ASSERT_TRUE(provision_config_directories(paths, fs, bundle, events).has_value());

    EXPECT_EQ(fs.files[paths.config_files_directory + "fastecu.cfg"],
              (std::vector<std::uint8_t>{1, 2, 3}));
}

TEST(ProvisionConfigDirectories, PrunesSyslogsKeepingNewest20)
{
    FakeFileSystem fs;
    FakeResourceBundle bundle;
    RecordingEventSink events;
    ConfigPaths paths = test_paths();
    fs.create_directory(paths.syslog_files_directory);
    for (int i = 0; i < 25; ++i)
    {
        std::string name = "log" + std::to_string(i) + ".txt";
        fs.files[paths.syslog_files_directory + name] = {};
        fs.files_by_parent[paths.syslog_files_directory].push_back({name, i});
    }

    ASSERT_TRUE(provision_config_directories(paths, fs, bundle, events).has_value());

    int remaining = 0;
    for (auto& [path, bytes] : fs.files)
        if (path.starts_with(paths.syslog_files_directory))
            ++remaining;
    EXPECT_EQ(remaining, 20);
    // The 5 oldest (mtime 0..4) are the ones removed.
    for (int i = 0; i < 5; ++i)
        EXPECT_FALSE(fs.exists(paths.syslog_files_directory + "log" + std::to_string(i) + ".txt"));
    for (int i = 5; i < 25; ++i)
        EXPECT_TRUE(fs.exists(paths.syslog_files_directory + "log" + std::to_string(i) + ".txt"));
}

TEST(ProvisionConfigDirectories, FirstCreateDirectoryFailureStopsTheSequence)
{
    class FailingFileSystem : public FakeFileSystem
    {
      public:
        Status create_directory(std::string_view path) override
        {
            if (std::string(path) == "/base")
                return fastecu::fail(ErrorKind::Internal, "permission denied");
            return FakeFileSystem::create_directory(path);
        }
    } fs;
    FakeResourceBundle bundle;
    RecordingEventSink events;
    ConfigPaths paths = test_paths();

    auto result = provision_config_directories(paths, fs, bundle, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_FALSE(fs.exists(paths.calibration_files_directory));
}
