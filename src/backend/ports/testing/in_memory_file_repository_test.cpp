#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

#include <gtest/gtest.h>

TEST(InMemoryFileRepository, WriteThenReadRoundTrips)
{
    fastecu::InMemoryFileRepository repo;
    std::vector<std::uint8_t> data = {1, 2, 3};
    ASSERT_THAT(repo.write("rom", data), fastecu::testing::IsOk());
    auto r = repo.read("rom");
    ASSERT_THAT(r, fastecu::testing::IsOkAnd(data));
}

TEST(InMemoryFileRepository, MissingHandleIsInvalidConfig)
{
    fastecu::InMemoryFileRepository repo;
    auto r = repo.read("absent");
    ASSERT_THAT(r, fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(InMemoryFileRepository, RecordsAndCanOverrideNextRead)
{
    fastecu::InMemoryFileRepository repository;
    repository.next_read_result = fastecu::fail(fastecu::ErrorKind::Internal, "disk error");

    auto result = repository.read("kernel");

    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_EQ(repository.read_handles, std::vector<std::string>{"kernel"});
}

TEST(InMemoryFileRepository, ReadOverrideIsConsumedAfterOneRead)
{
    fastecu::InMemoryFileRepository repository;
    repository.files["kernel"] = {0xaa, 0xbb};
    repository.next_read_result = fastecu::fail(fastecu::ErrorKind::Internal, "disk error");

    ASSERT_THAT(repository.read("kernel"), ::testing::Not(fastecu::testing::IsOk()));
    auto result = repository.read("kernel");

    ASSERT_THAT(result, fastecu::testing::IsOkAnd((std::vector<std::uint8_t>{0xaa, 0xbb})));
    EXPECT_EQ(repository.read_handles, (std::vector<std::string>{"kernel", "kernel"}));
}

TEST(InMemoryFileRepository, RecordsOrderedWritesAndStoresBothData)
{
    fastecu::InMemoryFileRepository repository;
    const std::vector<std::uint8_t> rom_data = {1, 2, 3};
    const std::vector<std::uint8_t> kernel_data = {4, 5, 6};

    ASSERT_THAT(repository.write("rom", rom_data), fastecu::testing::IsOk());
    ASSERT_THAT(repository.write("kernel", kernel_data), fastecu::testing::IsOk());

    EXPECT_EQ(repository.write_calls, (std::vector<std::pair<std::string, std::vector<std::uint8_t>>>{
                                          {"rom", {1, 2, 3}}, {"kernel", {4, 5, 6}}}));
    EXPECT_EQ(repository.files["rom"], rom_data);
    EXPECT_EQ(repository.files["kernel"], kernel_data);
}

TEST(InMemoryFileRepository, PersistentReadErrorAppliesAfterOneShotOverride)
{
    fastecu::InMemoryFileRepository repository;
    repository.files["kernel"] = {0xaa};
    repository.read_errors.insert_or_assign("kernel", fastecu::Error{fastecu::ErrorKind::Internal, "persistent error"});
    repository.next_read_result = std::vector<std::uint8_t>{0xbb};

    auto first = repository.read("kernel");
    auto second = repository.read("kernel");

    ASSERT_THAT(first, fastecu::testing::IsOkAnd((std::vector<std::uint8_t>{0xbb})));
    ASSERT_THAT(second, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_EQ(second.error(), repository.read_errors.at("kernel"));
    EXPECT_EQ(repository.read_count("kernel"), 2);
}

TEST(InMemoryFileRepository, ReadCountIsPerHandleAndIncludesFailures)
{
    fastecu::InMemoryFileRepository repository;
    repository.read_errors.insert_or_assign("broken", fastecu::Error{fastecu::ErrorKind::Internal, "broken"});

    EXPECT_THAT(repository.read("broken"), ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(repository.read("missing"), ::testing::Not(fastecu::testing::IsOk()));

    EXPECT_EQ(repository.read_count("broken"), 1);
    EXPECT_EQ(repository.read_count("missing"), 1);
    EXPECT_EQ(repository.read_count("unread"), 0);
}
