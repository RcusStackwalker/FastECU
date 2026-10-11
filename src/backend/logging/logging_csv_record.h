#pragma once

#include <span>
#include <string>
#include <string_view>

namespace fastecu::logging
{
std::string SerializeLoggingCsvRecord(std::span<const std::string_view> fields);
} // namespace fastecu::logging
