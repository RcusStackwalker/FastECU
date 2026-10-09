#include "src/backend/logging/logging_csv_record.h"

#include <array>
#include <string_view>
#include <gtest/gtest.h>

namespace fastecu::logging
{
TEST(LoggingCsvRecord, PreservesColumnOrderEmptyFieldsAndTrailingComma)
{
    const std::array<std::string_view, 3> fields{"Time", "RPM", "Flag"};
    EXPECT_EQ(SerializeLoggingCsvRecord(fields), "Time,RPM,Flag,\n");
    const std::array<std::string_view, 3> empty{"", "rpm", ""};
    EXPECT_EQ(SerializeLoggingCsvRecord(empty), ",rpm,,\n");
}

TEST(LoggingCsvRecord, QuotesCommasQuotesLineBreaksAndPreservesUtf8)
{
    const std::array<std::string_view, 5> fields{"a,b", "a\"b", "a\rb", "a\nb", "温度"};
    EXPECT_EQ(SerializeLoggingCsvRecord(fields), "\"a,b\",\"a\"\"b\",\"a\rb\",\"a\nb\",温度,\n");
}

TEST(LoggingCsvRecord, EmptyRecordContainsOnlyLineEnding)
{
    EXPECT_EQ(SerializeLoggingCsvRecord({}), "\n");
}
} // namespace fastecu::logging
