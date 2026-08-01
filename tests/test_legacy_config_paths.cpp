// tests/test_legacy_config_paths.cpp
#include "src/backend/config/legacy_config_paths.h"

#include <gtest/gtest.h>

namespace fastecu::config
{
namespace
{

// All twelve fields, each with a distinct value: a copy-paste slip that
// assigned the same source field twice would pass a test that reused values.
TEST(LegacyConfigPathsTest, EveryFieldIsCarriedAcross)
{
    fastecu::definitions::ConfigValuesStructure values;
    values.base_config_directory = "base/";
    values.version_config_directory = "version/";
    values.calibration_files_directory = "calibrations/";
    values.config_files_directory = "config/";
    values.definition_files_directory = "definitions/";
    values.kernel_files_directory = "kernels/";
    values.datalog_files_directory = "datalogs/";
    values.syslog_files_directory = "syslogs/";
    values.config_file = "fastecu.cfg";
    values.menu_file = "menu.cfg";
    values.protocols_file = "protocols.cfg";
    values.logger_file = "logger.cfg";

    const ConfigPaths paths = paths_from_config_values(values);

    EXPECT_EQ(paths.base_config_directory, "base/");
    EXPECT_EQ(paths.version_config_directory, "version/");
    EXPECT_EQ(paths.calibration_files_directory, "calibrations/");
    EXPECT_EQ(paths.config_files_directory, "config/");
    EXPECT_EQ(paths.definition_files_directory, "definitions/");
    EXPECT_EQ(paths.kernel_files_directory, "kernels/");
    EXPECT_EQ(paths.datalog_files_directory, "datalogs/");
    EXPECT_EQ(paths.syslog_files_directory, "syslogs/");
    EXPECT_EQ(paths.config_file, "fastecu.cfg");
    EXPECT_EQ(paths.menu_file, "menu.cfg");
    EXPECT_EQ(paths.protocols_file, "protocols.cfg");
    EXPECT_EQ(paths.logger_file, "logger.cfg");
}

TEST(LegacyConfigPathsTest, EmptyStructYieldsEmptyPaths)
{
    const fastecu::definitions::ConfigValuesStructure values;
    const ConfigPaths paths = paths_from_config_values(values);
    // Deliberately not protocols_file: ConfigValuesStructure gives it a
    // compiled-in default ("protocols.cfg", config_values.h) that is never
    // blank, unlike the *_directory fields and kernel_files_directory used
    // here, which have no default member initializer.
    EXPECT_TRUE(paths.datalog_files_directory.empty());
    EXPECT_TRUE(paths.kernel_files_directory.empty());
}

} // namespace
} // namespace fastecu::config

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
