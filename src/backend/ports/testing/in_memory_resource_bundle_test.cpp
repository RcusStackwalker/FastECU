#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/ports/testing/in_memory_resource_bundle.h"

#include <gtest/gtest.h>

TEST(ResourceBundle, ListReturnsAllNames)
{
    fastecu::InMemoryResourceBundle bundle;
    bundle.bundles["config"]["fastecu.cfg"] = {'a'};
    bundle.bundles["config"]["menu.cfg"] = {'b'};
    auto names = bundle.list("config");
    ASSERT_THAT(names, fastecu::testing::IsOk());
    EXPECT_EQ(names->size(), 2U);
}

TEST(ResourceBundle, ReadUnknownBundleIsInvalidConfig)
{
    fastecu::InMemoryResourceBundle bundle;
    ASSERT_THAT(bundle.read("kernels", "missing.bin"), fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(ResourceBundle, ReadKnownFileRoundTrips)
{
    fastecu::InMemoryResourceBundle bundle;
    bundle.bundles["kernels"]["k.bin"] = {1, 2, 3};
    ASSERT_THAT(bundle.read("kernels", "k.bin"), fastecu::testing::IsOkAnd((std::vector<std::uint8_t>{1, 2, 3})));
}
