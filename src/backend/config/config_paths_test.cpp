#include "src/backend/config/config_paths.h"
#include <gtest/gtest.h>

using fastecu::config::ConfigPaths;
using fastecu::config::ResolveConfigPaths;

TEST(ResolveConfigPaths, PathsContainingBuildStillNestUnderVersionDirectory)
{
    ConfigPaths paths = ResolveConfigPaths("/home/user/project/build", "0.1.0-beta.5");

    EXPECT_EQ(paths.base_config_directory, "/home/user/project/build");
    EXPECT_EQ(paths.version_config_directory, "/home/user/project/build/0.1.0-beta.5/");
    EXPECT_EQ(paths.calibration_files_directory, "/home/user/project/build/0.1.0-beta.5/calibrations/");
    EXPECT_EQ(paths.config_files_directory, "/home/user/project/build/0.1.0-beta.5/config/");
    EXPECT_EQ(paths.definition_files_directory, "/home/user/project/build/0.1.0-beta.5/definitions/");
    EXPECT_EQ(paths.kernel_files_directory, "/home/user/project/build/0.1.0-beta.5/kernels/");
    EXPECT_EQ(paths.datalog_files_directory, "/home/user/project/build/0.1.0-beta.5/datalogs/");
    EXPECT_EQ(paths.syslog_files_directory, "/home/user/project/build/0.1.0-beta.5/syslogs/");
    EXPECT_EQ(paths.config_file, "/home/user/project/build/0.1.0-beta.5/config/fastecu.cfg");
    EXPECT_EQ(paths.logger_file, "/home/user/project/build/0.1.0-beta.5/config/logger.cfg");
}

TEST(ResolveConfigPaths, InstalledPathNestsUnderVersionDirectory)
{
    ConfigPaths paths = ResolveConfigPaths("/home/user/.config/FastECU", "0.1.0-beta.5");

    EXPECT_EQ(paths.base_config_directory, "/home/user/.config/FastECU");
    EXPECT_EQ(paths.version_config_directory, "/home/user/.config/FastECU/0.1.0-beta.5/");
    EXPECT_EQ(paths.calibration_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/calibrations/");
    EXPECT_EQ(paths.config_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/config/");
    EXPECT_EQ(paths.definition_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/definitions/");
    EXPECT_EQ(paths.kernel_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/kernels/");
    EXPECT_EQ(paths.datalog_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/datalogs/");
    EXPECT_EQ(paths.syslog_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/syslogs/");
    EXPECT_EQ(paths.config_file, "/home/user/.config/FastECU/0.1.0-beta.5/config/fastecu.cfg");
}
