#include "src/backend/logging/logging_csv_record.h"

namespace fastecu::logging
{
std::string serialize_logging_csv_record(std::span<const std::string_view> fields)
{
    std::string record;
    for (const auto field : fields)
    {
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
        record.push_back(',');
    }
    record.push_back('\n');
    return record;
}
} // namespace fastecu::logging
