#include "src/backend/logging/logging_csv_record.h"

#include <array>
#include <string_view>
#include <gtest/gtest.h>

namespace fastecu::logging
{
TEST(LoggingCsvRecordTest, PreservesColumnOrderEmptyFieldsAndTrailingComma)
{
    const std::array<std::string_view, 3> fields{"Time", "RPM", "Flag"};
    EXPECT_EQ(serialize_logging_csv_record(fields), "Time,RPM,Flag,\n");
    const std::array<std::string_view, 3> empty{"", "rpm", ""};
    EXPECT_EQ(serialize_logging_csv_record(empty), ",rpm,,\n");
}
TEST(LoggingCsvRecordTest, QuotesCommasQuotesLineBreaksAndPreservesUtf8)
{
    const std::array<std::string_view, 5> fields{"a,b", "a\"b", "a\rb", "a\nb", "温度"};
    EXPECT_EQ(serialize_logging_csv_record(fields), "\"a,b\",\"a\"\"b\",\"a\rb\",\"a\nb\",温度,\n");
}
} // namespace fastecu::logging
