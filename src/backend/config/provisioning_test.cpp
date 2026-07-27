#include "src/backend/config/provisioning.h"
#include "src/backend/ports/testing/in_memory_file_system.h"
#include "src/backend/ports/testing/in_memory_resource_bundle.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <map>

using fastecu::DirEntry;
using fastecu::ErrorKind;
using fastecu::InMemoryFileSystem;
using fastecu::InMemoryResourceBundle;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::Result;
using fastecu::Status;
using fastecu::config::ConfigPaths;
using fastecu::config::provision_config_directories;

namespace
{
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
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
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
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    RecordingEventSink events;
    ConfigPaths paths = test_paths();

    ASSERT_TRUE(provision_config_directories(paths, fs, bundle, events).has_value());
    auto directories_after_first = fs.directories;
    ASSERT_TRUE(provision_config_directories(paths, fs, bundle, events).has_value());
    EXPECT_EQ(fs.directories, directories_after_first);
}

TEST(ProvisionConfigDirectories, CopiesBundledResourceFilesNotAlreadyPresent)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
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
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
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
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
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

TEST(ProvisionConfigDirectories, BundleCopyFailurePropagatesRatherThanBeingSwallowed)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    RecordingEventSink events;
    ConfigPaths paths = test_paths();
    // A bundled kernel file nested under a subdirectory that provisioning
    // never creates. Its destination's parent directory doesn't exist, so
    // copy_file fails for a genuine I/O-style reason -- not the "already
    // exists" case the fs.exists(target) pre-check already carves out as
    // non-fatal -- and that failure must propagate, not be swallowed.
    bundle.bundles["kernels"]["missing_subdir/k2.bin"] = {3};
    fs.files["kernels/missing_subdir/k2.bin"] = {3};

    auto result = provision_config_directories(paths, fs, bundle, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST(ProvisionConfigDirectories, MigratesPreviousVersionConfigFileForward)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    RecordingEventSink events;
    ConfigPaths paths = test_paths();
    // A previous-version directory "0.9" already exists under base, newer
    // than nothing else, with its own config/fastecu.cfg.
    fs.subdirectories_by_parent[paths.base_config_directory].push_back({"0.9", 100});
    fs.files[paths.base_config_directory + "/0.9/config/fastecu.cfg"] = {7, 7, 7};

    ASSERT_TRUE(provision_config_directories(paths, fs, bundle, events).has_value());

    ASSERT_TRUE(fs.exists(paths.config_files_directory + "fastecu.cfg"));
    EXPECT_EQ(fs.files[paths.config_files_directory + "fastecu.cfg"], (std::vector<std::uint8_t>{7, 7, 7}));
}

TEST(ProvisionConfigDirectories, FirstCreateDirectoryFailureStopsTheSequence)
{
    class FailingFileSystem : public InMemoryFileSystem
    {
      public:
        Status create_directory(std::string_view path) override
        {
            if (path == "/base"sv)
                return fastecu::fail(ErrorKind::Internal, "permission denied");
            return InMemoryFileSystem::create_directory(path);
        }
    } fs;
    InMemoryResourceBundle bundle;
    RecordingEventSink events;
    ConfigPaths paths = test_paths();

    auto result = provision_config_directories(paths, fs, bundle, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_FALSE(fs.exists(paths.calibration_files_directory));
}
