#pragma once

#include <span>
#include <string>
#include <string_view>

namespace fastecu::logging
{
std::string serialize_logging_csv_record(std::span<const std::string_view> fields);
} // namespace fastecu::logging
