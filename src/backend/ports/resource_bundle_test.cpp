#include "src/backend/ports/resource_bundle.h"
#include <gtest/gtest.h>
#include <map>

using fastecu::ErrorKind;
using fastecu::IResourceBundle;
using fastecu::Result;

namespace
{
class InMemoryResourceBundle : public IResourceBundle
{
  public:
    Result<std::vector<std::string>> list(std::string_view bundle_id) override
    {
        auto it = bundles.find(std::string(bundle_id));
        if (it == bundles.end())
            return fastecu::fail(ErrorKind::InvalidConfig, "no such bundle");
        std::vector<std::string> names;
        for (auto& [name, bytes] : it->second)
            names.push_back(name);
        return names;
    }
    Result<std::vector<std::uint8_t>> read(std::string_view bundle_id, std::string_view name) override
    {
        auto bundle_it = bundles.find(std::string(bundle_id));
        if (bundle_it == bundles.end())
            return fastecu::fail(ErrorKind::InvalidConfig, "no such bundle");
        auto file_it = bundle_it->second.find(std::string(name));
        if (file_it == bundle_it->second.end())
            return fastecu::fail(ErrorKind::InvalidConfig, "no such file");
        return file_it->second;
    }

    std::map<std::string, std::map<std::string, std::vector<std::uint8_t>>> bundles;
};
} // namespace

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
