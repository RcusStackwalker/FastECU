#include "src/backend/ports/testing/result_matchers.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include <gtest/gtest.h>
#include <algorithm>

using fastecu::ErrorKind;

TEST(QtResourceBundleTest, ListsRealShippedConfigFiles)
{
    QtResourceBundle bundle;
    auto names = bundle.list("config");

    ASSERT_THAT(names, fastecu::testing::IsOk());
    EXPECT_NE(std::find(names->begin(), names->end(), "fastecu.cfg"), names->end());
    EXPECT_NE(std::find(names->begin(), names->end(), "protocols.cfg"), names->end());
}

TEST(QtResourceBundleTest, ListsRealShippedKernelFiles)
{
    QtResourceBundle bundle;
    auto names = bundle.list("kernels");

    ASSERT_THAT(names, fastecu::testing::IsOk());
    EXPECT_NE(std::find(names->begin(), names->end(), "ssmk_can_sh7055.bin"), names->end());
}

TEST(QtResourceBundleTest, ReadReturnsNonEmptyBytesForAKnownFile)
{
    QtResourceBundle bundle;
    auto bytes = bundle.read("config", "fastecu.cfg");

    ASSERT_THAT(bytes, fastecu::testing::IsOk());
    EXPECT_GT(bytes->size(), 0U);
}

TEST(QtResourceBundleTest, UnknownBundleIdIsInvalidConfig)
{
    QtResourceBundle bundle;
    ASSERT_THAT(bundle.list("not-a-real-bundle"), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}
