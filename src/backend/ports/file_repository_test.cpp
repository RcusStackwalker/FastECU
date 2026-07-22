#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/settings.h"
#include <gtest/gtest.h>
#include <map>
#include <string>

using fastecu::ErrorKind;
using fastecu::IFileRepository;
using fastecu::ISettings;
using fastecu::Result;
using fastecu::Status;

namespace
{
class InMemoryFileRepository : public IFileRepository
{
  public:
    Result<std::vector<std::uint8_t>> read(std::string_view h) override
    {
        auto it = files.find(std::string(h));
        if (it == files.end())
            return fastecu::fail(ErrorKind::InvalidConfig, "no such handle");
        return it->second;
    }
    Status write(std::string_view h, std::span<const std::uint8_t> d) override
    {
        files[std::string(h)].assign(d.begin(), d.end());
        return {};
    }
    std::map<std::string, std::vector<std::uint8_t>> files;
};

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

TEST(FileRepository, WriteThenReadRoundTrips)
{
    InMemoryFileRepository repo;
    std::vector<std::uint8_t> data = {1, 2, 3};
    ASSERT_TRUE(repo.write("rom", data).has_value());
    auto r = repo.read("rom");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, data);
}

TEST(FileRepository, MissingHandleIsInvalidConfig)
{
    InMemoryFileRepository repo;
    auto r = repo.read("absent");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::InvalidConfig);
}

TEST(Settings, GetMissingReturnsNullopt)
{
    InMemorySettings s;
    EXPECT_FALSE(s.get("k").has_value());
    s.set("k", "v");
    ASSERT_TRUE(s.get("k").has_value());
    EXPECT_EQ(*s.get("k"), "v");
}
