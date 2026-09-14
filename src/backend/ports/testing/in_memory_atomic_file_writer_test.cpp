#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"

#include <gtest/gtest.h>

TEST(InMemoryAtomicFileWriter, SuccessfulReplacementRecordsCallAndStoresContents)
{
    fastecu::InMemoryAtomicFileWriter writer;
    const std::vector<std::uint8_t> data{1, 2, 3};

    ASSERT_THAT(writer.replace("definition.xml", data), fastecu::testing::IsOk());

    ASSERT_EQ(writer.replace_calls.size(), 1U);
    EXPECT_EQ(writer.replace_calls[0].handle, "definition.xml");
    EXPECT_EQ(writer.replace_calls[0].data, data);
    EXPECT_EQ(writer.files.at("definition.xml"), data);
}

TEST(InMemoryAtomicFileWriter, FailedReplacementIsRecordedWithoutUpdatingContents)
{
    fastecu::InMemoryAtomicFileWriter writer;
    writer.files["definition.xml"] = {1};
    writer.replace_error = fastecu::Error{fastecu::ErrorKind::Internal, "replace failed"};

    ASSERT_THAT(writer.replace("definition.xml", std::vector<std::uint8_t>{2}),
                ::testing::Not(fastecu::testing::IsOk()));
    ASSERT_EQ(writer.replace_calls.size(), 1U);
    EXPECT_EQ(writer.replace_calls[0].data, (std::vector<std::uint8_t>{2}));
    EXPECT_EQ(writer.files.at("definition.xml"), (std::vector<std::uint8_t>{1}));
}

TEST(InMemoryAtomicFileWriter, ResetClearsCallsFilesAndInjectedError)
{
    fastecu::InMemoryAtomicFileWriter writer;
    writer.files["definition.xml"] = {1};
    writer.replace_calls.push_back({"definition.xml", {2}});
    writer.replace_error = fastecu::Error{fastecu::ErrorKind::Internal, "replace failed"};

    writer.reset();

    EXPECT_TRUE(writer.files.empty());
    EXPECT_TRUE(writer.replace_calls.empty());
    EXPECT_FALSE(writer.replace_error.has_value());
}
