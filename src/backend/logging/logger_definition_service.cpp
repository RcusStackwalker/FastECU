#include "src/backend/logging/logger_definition_service.h"

#include <format>

namespace fastecu::logging
{
namespace
{

constexpr std::string_view kCdbgProtocol = "CDBG";
constexpr std::string_view kCdbgExampleName = "logger_cdbg_example.xml";
constexpr std::string_view kBundledCdbgExample = ":/config/logger_cdbg_example.xml";

} // namespace

LoggerDefinitionService::LoggerDefinitionService(IFileRepository& repository, IAtomicFileWriter& writer)
    : repository_(repository), writer_(writer)
{
}

std::string LoggerDefinitionService::ResolveDefinitionHandle(std::string_view configured_handle,
                                                             std::string_view log_protocol,
                                                             std::string_view config_files_directory)
{
    if (!configured_handle.empty())
    {
        return std::string(configured_handle);
    }
    if (log_protocol != kCdbgProtocol)
    {
        return std::string{};
    }

    if (const std::string user_copy = std::format("{}{}", config_files_directory, kCdbgExampleName);
        repository_.Read(user_copy).has_value())
    {
        return user_copy;
    }
    return std::string(kBundledCdbgExample);
}

Result<LoggerDefinition> LoggerDefinitionService::LoadDefinition(std::string_view handle)
{
    auto contents = repository_.Read(handle);
    if (!contents)
    {
        return std::unexpected(contents.error());
    }
    return ParseLoggerDefinition(*contents, handle);
}

Result<std::optional<LoggerSelection>> LoggerDefinitionService::LoadSelection(std::string_view conf_handle,
                                                                              std::string_view ecu_id)
{
    auto contents = repository_.Read(conf_handle);
    if (!contents)
    {
        return std::unexpected(contents.error());
    }
    return ReadSelection(*contents, ecu_id, conf_handle);
}

Status LoggerDefinitionService::SaveSelection(std::string_view conf_handle, std::string_view ecu_id,
                                              const LoggerSelection& selection)
{
    auto contents = repository_.Read(conf_handle);
    if (!contents)
    {
        return std::unexpected(contents.error());
    }
    auto written = WriteSelection(*contents, ecu_id, selection, conf_handle);
    if (!written)
    {
        return std::unexpected(written.error());
    }
    return writer_.Replace(conf_handle, *written);
}

} // namespace fastecu::logging
