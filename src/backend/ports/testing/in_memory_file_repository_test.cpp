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

TEST(InMemoryFileRepository, RecordsAndCanOverrideNextRead)
{
    fastecu::InMemoryFileRepository repository;
    repository.next_read_result = fastecu::fail(fastecu::ErrorKind::Internal, "disk error");

    auto result = repository.read("kernel");

    ASSERT_FALSE(result);
    EXPECT_EQ(repository.read_handles, std::vector<std::string>{"kernel"});
}

TEST(InMemoryFileRepository, ReadOverrideIsConsumedAfterOneRead)
{
    fastecu::InMemoryFileRepository repository;
    repository.files["kernel"] = {0xaa, 0xbb};
    repository.next_read_result = fastecu::fail(fastecu::ErrorKind::Internal, "disk error");

    ASSERT_FALSE(repository.read("kernel"));
    auto result = repository.read("kernel");

    ASSERT_TRUE(result);
    EXPECT_EQ(*result, (std::vector<std::uint8_t>{0xaa, 0xbb}));
    EXPECT_EQ(repository.read_handles, (std::vector<std::string>{"kernel", "kernel"}));
}

TEST(InMemoryFileRepository, RecordsWriteBeforeStoringData)
{
    fastecu::InMemoryFileRepository repository;
    const std::vector<std::uint8_t> data = {1, 2, 3};

    ASSERT_TRUE(repository.write("rom", data));

    EXPECT_EQ(repository.write_calls,
              (std::vector<std::pair<std::string, std::vector<std::uint8_t>>>{{"rom", {1, 2, 3}}}));
    EXPECT_EQ(repository.files["rom"], data);
}
