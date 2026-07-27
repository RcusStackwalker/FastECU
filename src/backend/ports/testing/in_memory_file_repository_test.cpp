#include "src/backend/ports/testing/in_memory_file_repository.h"

#include <gtest/gtest.h>

TEST(InMemoryFileRepository, WriteThenReadRoundTrips)
{
    fastecu::InMemoryFileRepository repo;
    std::vector<std::uint8_t> data = {1, 2, 3};
    ASSERT_TRUE(repo.write("rom", data).has_value());
    auto r = repo.read("rom");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, data);
}

TEST(InMemoryFileRepository, MissingHandleIsInvalidConfig)
{
    fastecu::InMemoryFileRepository repo;
    auto r = repo.read("absent");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, fastecu::ErrorKind::InvalidConfig);
}
