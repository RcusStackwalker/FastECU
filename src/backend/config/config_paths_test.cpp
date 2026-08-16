#include "src/backend/config/config_paths.h"
#include <gtest/gtest.h>

using fastecu::config::AppRootInfo;
using fastecu::config::ConfigPaths;
using fastecu::config::resolve_config_paths;

TEST(ResolveConfigPaths, DevPathUsesRootDirectlyWithoutVersionSubdirectory)
{
    AppRootInfo root{"/home/user/project/build", true};
    ConfigPaths paths = resolve_config_paths(root, "0.1.0-beta.5");

    EXPECT_EQ(paths.base_config_directory, "/home/user/project/build");
    EXPECT_EQ(paths.version_config_directory, "/home/user/project/build");
    EXPECT_EQ(paths.calibration_files_directory, "/home/user/project/build/calibrations/");
    EXPECT_EQ(paths.config_files_directory, "/home/user/project/build/config/");
    EXPECT_EQ(paths.definition_files_directory, "/home/user/project/build/definitions/");
    EXPECT_EQ(paths.kernel_files_directory, "/home/user/project/build/kernels/");
    EXPECT_EQ(paths.datalog_files_directory, "/home/user/project/build/datalogs/");
    EXPECT_EQ(paths.syslog_files_directory, "/home/user/project/build/syslogs/");
    EXPECT_EQ(paths.config_file, "/home/user/project/build/config/fastecu.cfg");
    EXPECT_EQ(paths.menu_file, "/home/user/project/build/config/menu.cfg");
    EXPECT_EQ(paths.protocols_file, "/home/user/project/build/config/protocols.cfg");
    EXPECT_EQ(paths.logger_file, "/home/user/project/build/config/logger.cfg");
}

TEST(ResolveConfigPaths, InstalledPathNestsUnderVersionDirectory)
{
    AppRootInfo root{"/home/user/.config/FastECU", false};
    ConfigPaths paths = resolve_config_paths(root, "0.1.0-beta.5");

    EXPECT_EQ(paths.base_config_directory, "/home/user/.config/FastECU");
    EXPECT_EQ(paths.version_config_directory, "/home/user/.config/FastECU/0.1.0-beta.5/");
    EXPECT_EQ(paths.calibration_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/calibrations/");
    EXPECT_EQ(paths.config_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/config/");
    EXPECT_EQ(paths.definition_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/definitions/");
    EXPECT_EQ(paths.kernel_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/kernels/");
    EXPECT_EQ(paths.datalog_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/datalogs/");
    EXPECT_EQ(paths.syslog_files_directory, "/home/user/.config/FastECU/0.1.0-beta.5/syslogs/");
    EXPECT_EQ(paths.config_file, "/home/user/.config/FastECU/0.1.0-beta.5/config/fastecu.cfg");
    EXPECT_EQ(paths.protocols_file, "/home/user/.config/FastECU/0.1.0-beta.5/config/protocols.cfg");
}
