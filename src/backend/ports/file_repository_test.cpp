#include "src/backend/ports/settings.h"
#include <gtest/gtest.h>
#include <map>
#include <string>

using fastecu::ISettings;

namespace
{
class InMemorySettings : public ISettings
{
  public:
    std::optional<std::string> get(std::string_view k) const override
    {
        auto it = kv.find(std::string(k));
        if (it == kv.end())
            return std::nullopt;
        return it->second;
    }
    void set(std::string_view k, std::string_view v) override
    {
        kv[std::string(k)] = std::string(v);
    }
    std::map<std::string, std::string> kv;
};
} // namespace

TEST(Settings, GetMissingReturnsNullopt)
{
    InMemorySettings s;
    EXPECT_FALSE(s.get("k").has_value());
    s.set("k", "v");
    ASSERT_TRUE(s.get("k").has_value());
    EXPECT_EQ(*s.get("k"), "v");
}
