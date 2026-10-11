#include "src/backend/logging/logging_csv_record.h"

namespace fastecu::logging
{
std::string SerializeLoggingCsvRecord(std::span<const std::string_view> fields)
{
    std::string record;
    bool first = true;
    for (const auto field : fields)
    {
        if (!first)
        {
            record.push_back(',');
        }
        first = false;
        const bool quote = field.find_first_of(",\"\r\n") != std::string_view::npos;
        if (quote)
        {
            record.push_back('"');
        }
        for (const auto character : field)
        {
            if (character == '"')
            {
                record.push_back('"');
            }
            record.push_back(character);
        }
        if (quote)
        {
            record.push_back('"');
        }
    }
    record.push_back('\n');
    return record;
}
} // namespace fastecu::logging
