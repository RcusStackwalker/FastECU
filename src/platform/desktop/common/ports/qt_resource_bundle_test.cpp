#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include <gtest/gtest.h>
#include <algorithm>

using fastecu::ErrorKind;

TEST(QtResourceBundleTest, ListsRealShippedConfigFiles)
{
    QtResourceBundle bundle;
    auto names = bundle.list("config");

    ASSERT_TRUE(names.has_value());
    EXPECT_NE(std::find(names->begin(), names->end(), "fastecu.cfg"), names->end());
    EXPECT_NE(std::find(names->begin(), names->end(), "protocols.cfg"), names->end());
}

TEST(QtResourceBundleTest, ListsRealShippedKernelFiles)
{
    QtResourceBundle bundle;
    auto names = bundle.list("kernels");

    ASSERT_TRUE(names.has_value());
    EXPECT_NE(std::find(names->begin(), names->end(), "ssmk_can_sh7055.bin"), names->end());
}

TEST(QtResourceBundleTest, ReadReturnsNonEmptyBytesForAKnownFile)
{
    QtResourceBundle bundle;
    auto bytes = bundle.read("config", "fastecu.cfg");

    ASSERT_TRUE(bytes.has_value());
    EXPECT_GT(bytes->size(), 0U);
}

TEST(QtResourceBundleTest, UnknownBundleIdIsInvalidConfig)
{
    QtResourceBundle bundle;
    auto names = bundle.list("not-a-real-bundle");

    ASSERT_FALSE(names.has_value());
    EXPECT_EQ(names.error().kind, ErrorKind::InvalidConfig);
}
