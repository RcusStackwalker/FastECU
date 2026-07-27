#include "src/backend/ports/testing/in_memory_resource_bundle.h"

#include <gtest/gtest.h>

using fastecu::ErrorKind;
using fastecu::InMemoryResourceBundle;
using fastecu::Result;

TEST(ResourceBundle, ListReturnsAllNames)
{
    InMemoryResourceBundle bundle;
    bundle.bundles["config"]["fastecu.cfg"] = {'a'};
    bundle.bundles["config"]["menu.cfg"] = {'b'};
    auto names = bundle.list("config");
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(names->size(), 2u);
}

TEST(ResourceBundle, ReadUnknownBundleIsInvalidConfig)
{
    InMemoryResourceBundle bundle;
    auto r = bundle.read("kernels", "missing.bin");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::InvalidConfig);
}

TEST(ResourceBundle, ReadKnownFileRoundTrips)
{
    InMemoryResourceBundle bundle;
    bundle.bundles["kernels"]["k.bin"] = {1, 2, 3};
    auto r = bundle.read("kernels", "k.bin");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, (std::vector<std::uint8_t>{1, 2, 3}));
}
