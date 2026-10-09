#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/config/provisioning.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_file_system.h"
#include "src/backend/ports/testing/in_memory_resource_bundle.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <map>

using fastecu::DirEntry;
using fastecu::ErrorKind;
using fastecu::InMemoryFileRepository;
using fastecu::InMemoryFileSystem;
using fastecu::InMemoryResourceBundle;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::config::ConfigPaths;
using fastecu::config::ProvisionConfigDirectories;

namespace
{
ConfigPaths TestPaths()
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
    InMemoryFileRepository repo;
    RecordingEventSink events;
    ConfigPaths paths = TestPaths();

    ASSERT_THAT(ProvisionConfigDirectories(paths, fs, bundle, repo, events), fastecu::testing::IsOk());

    EXPECT_TRUE(fs.Exists(paths.base_config_directory));
    EXPECT_TRUE(fs.Exists(paths.calibration_files_directory));
    EXPECT_TRUE(fs.Exists(paths.config_files_directory));
    EXPECT_TRUE(fs.Exists(paths.definition_files_directory));
    EXPECT_TRUE(fs.Exists(paths.kernel_files_directory));
    EXPECT_TRUE(fs.Exists(paths.datalog_files_directory));
    EXPECT_TRUE(fs.Exists(paths.syslog_files_directory));
}

TEST(ProvisionConfigDirectories, IdempotentOnSecondRun)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    RecordingEventSink events;
    ConfigPaths paths = TestPaths();

    ASSERT_THAT(ProvisionConfigDirectories(paths, fs, bundle, repo, events), fastecu::testing::IsOk());
    auto directories_after_first = fs.directories;
    ASSERT_THAT(ProvisionConfigDirectories(paths, fs, bundle, repo, events), fastecu::testing::IsOk());
    EXPECT_EQ(fs.directories, directories_after_first);
}

TEST(ProvisionConfigDirectories, CopiesBundledResourceFilesNotAlreadyPresent)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    RecordingEventSink events;
    ConfigPaths paths = TestPaths();
    bundle.bundles["config"]["fastecu.cfg"] = {1};
    bundle.bundles["kernels"]["k1.bin"] = {2};

    ASSERT_THAT(ProvisionConfigDirectories(paths, fs, bundle, repo, events), fastecu::testing::IsOk());

    EXPECT_EQ(repo.files[paths.config_files_directory + "fastecu.cfg"], (std::vector<std::uint8_t>{1}));
    EXPECT_EQ(repo.files[paths.kernel_files_directory + "k1.bin"], (std::vector<std::uint8_t>{2}));
}

// The regression: bundled files used to be copied from "<bundle>/<name>", a
// path relative to the working directory that never reached the compiled-in
// (Qt ":/") resources. The bytes must come from the bundle itself, with no
// such relative file anywhere on disk.
TEST(ProvisionConfigDirectories, WritesTheBundledBytesWithoutARelativeSourceFile)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    RecordingEventSink events;
    ConfigPaths paths = TestPaths();
    bundle.bundles["config"]["fastecu.cfg"] = {0x3C, 0x63, 0x3E};
    ASSERT_FALSE(fs.Exists("config/fastecu.cfg"));

    ASSERT_THAT(ProvisionConfigDirectories(paths, fs, bundle, repo, events), fastecu::testing::IsOk());

    ASSERT_EQ(repo.write_calls.size(), 1U);
    EXPECT_EQ(repo.write_calls.front().first, paths.config_files_directory + "fastecu.cfg");
    EXPECT_EQ(repo.write_calls.front().second, (std::vector<std::uint8_t>{0x3C, 0x63, 0x3E}));
}

TEST(ProvisionConfigDirectories, DoesNotOverwriteAnExistingUserFile)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    RecordingEventSink events;
    ConfigPaths paths = TestPaths();
    bundle.bundles["config"]["fastecu.cfg"] = {9, 9, 9};
    ASSERT_THAT(fs.CreateDirectory(paths.config_files_directory), fastecu::testing::IsOk());
    fs.files[paths.config_files_directory + "fastecu.cfg"] = {1, 2, 3}; // user's own copy

    ASSERT_THAT(ProvisionConfigDirectories(paths, fs, bundle, repo, events), fastecu::testing::IsOk());

    EXPECT_EQ(fs.files[paths.config_files_directory + "fastecu.cfg"], (std::vector<std::uint8_t>{1, 2, 3}));
    EXPECT_TRUE(repo.write_calls.empty());
}

TEST(ProvisionConfigDirectories, PrunesSyslogsKeepingNewest20)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    RecordingEventSink events;
    ConfigPaths paths = TestPaths();
    ASSERT_THAT(fs.CreateDirectory(paths.syslog_files_directory), fastecu::testing::IsOk());
    for (int i = 0; i < 25; ++i)
    {
        std::string name = "log" + std::to_string(i) + ".txt";
        fs.files[paths.syslog_files_directory + name] = {};
        fs.directory_entries[paths.syslog_files_directory].push_back(
            DirEntry{.name = name, .is_directory = false, .modified_time_epoch_seconds = i});
    }

    ASSERT_THAT(ProvisionConfigDirectories(paths, fs, bundle, repo, events), fastecu::testing::IsOk());

    int remaining = 0;
    for (auto& [path, bytes] : fs.files)
    {
        if (path.starts_with(paths.syslog_files_directory))
        {
            ++remaining;
        }
    }
    EXPECT_EQ(remaining, 20);
    // The 5 oldest (mtime 0..4) are the ones removed.
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_FALSE(fs.Exists(paths.syslog_files_directory + "log" + std::to_string(i) + ".txt"));
    }
    for (int i = 5; i < 25; ++i)
    {
        EXPECT_TRUE(fs.Exists(paths.syslog_files_directory + "log" + std::to_string(i) + ".txt"));
    }
}

TEST(ProvisionConfigDirectories, MigratesPreviousVersionConfigFileForward)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    RecordingEventSink events;
    ConfigPaths paths = TestPaths();
    // A previous-version directory "0.9" already exists under base, newer
    // than nothing else, with its own config/fastecu.cfg.
    fs.directory_entries[paths.base_config_directory].push_back(
        DirEntry{.name = "0.9", .is_directory = true, .modified_time_epoch_seconds = 100});
    fs.files[paths.base_config_directory + "/0.9/config/fastecu.cfg"] = {7, 7, 7};

    ASSERT_THAT(ProvisionConfigDirectories(paths, fs, bundle, repo, events), fastecu::testing::IsOk());

    ASSERT_TRUE(fs.Exists(paths.config_files_directory + "fastecu.cfg"));
    EXPECT_EQ(fs.files[paths.config_files_directory + "fastecu.cfg"], (std::vector<std::uint8_t>{7, 7, 7}));
}

TEST(ProvisionConfigDirectories, FirstCreateDirectoryFailureStopsTheSequence)
{
    InMemoryFileSystem fs;
    fs.create_directory_error = fastecu::Error{ErrorKind::kInternal, "permission denied"};
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    RecordingEventSink events;
    ConfigPaths paths = TestPaths();

    ASSERT_THAT(ProvisionConfigDirectories(paths, fs, bundle, repo, events),
                fastecu::testing::IsErr(ErrorKind::kInternal));
    EXPECT_FALSE(fs.Exists(paths.calibration_files_directory));
}

TEST(ProvisionConfigDirectories, CreateDirectoryFailureNamesThePath)
{
    InMemoryFileSystem fs;
    fs.create_directory_error = fastecu::Error{ErrorKind::kInternal, "permission denied"};
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    RecordingEventSink events;
    ConfigPaths paths = TestPaths();

    EXPECT_THAT(ProvisionConfigDirectories(paths, fs, bundle, repo, events),
                fastecu::testing::IsErrWith(ErrorKind::kInternal,
                                            ::testing::AllOf(::testing::HasSubstr(paths.base_config_directory),
                                                             ::testing::HasSubstr("permission denied"))));
}

namespace
{
// Lists its files but cannot read them.
class UnreadableResourceBundle : public InMemoryResourceBundle
{
  public:
    fastecu::Result<std::vector<std::uint8_t>> Read(std::string_view, std::string_view) override
    {
        return fastecu::Fail(ErrorKind::kInternal, "resource unreadable");
    }
};
} // namespace

TEST(ProvisionConfigDirectories, BundleReadFailureNamesTheTarget)
{
    InMemoryFileSystem fs;
    UnreadableResourceBundle bundle;
    bundle.bundles["config"]["menu.cfg"] = {1};
    InMemoryFileRepository repo;
    RecordingEventSink events;
    ConfigPaths paths = TestPaths();

    EXPECT_THAT(
        ProvisionConfigDirectories(paths, fs, bundle, repo, events),
        fastecu::testing::IsErrWith(ErrorKind::kInternal,
                                    ::testing::AllOf(::testing::HasSubstr(paths.config_files_directory + "menu.cfg"),
                                                     ::testing::HasSubstr("resource unreadable"))));
    EXPECT_TRUE(repo.write_calls.empty());
}

TEST(ProvisionConfigDirectories, RepositoryWriteFailureNamesTheTarget)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    bundle.bundles["config"]["menu.cfg"] = {1};
    InMemoryFileRepository repo;
    ConfigPaths paths = TestPaths();
    repo.write_errors[paths.config_files_directory + "menu.cfg"] = fastecu::Error{ErrorKind::kInternal, "disk full"};
    RecordingEventSink events;

    EXPECT_THAT(
        ProvisionConfigDirectories(paths, fs, bundle, repo, events),
        fastecu::testing::IsErrWith(ErrorKind::kInternal,
                                    ::testing::AllOf(::testing::HasSubstr(paths.config_files_directory + "menu.cfg"),
                                                     ::testing::HasSubstr("disk full"))));
}
