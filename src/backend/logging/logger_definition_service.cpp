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

LoggerDefinitionService::LoggerDefinitionService(
    IFileRepository& repository, IResourceBundle& bundle, IAtomicFileWriter& writer)
    : repository_(repository), bundle_(bundle), writer_(writer)
{
}

Result<std::string> LoggerDefinitionService::resolve_definition_handle(
    std::string_view configured_handle,
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

    const std::string user_copy = std::format("{}{}", config_files_directory, kCdbgExampleName);
    if (repository_.read(user_copy).has_value())
    {
        return user_copy;
    }
    return std::string(kBundledCdbgExample);
}

Result<LoggerDefinition> LoggerDefinitionService::load_definition(std::string_view handle)
{
    auto contents = repository_.read(handle);
    if (!contents)
    {
        return std::unexpected(contents.error());
    }
    return parse_logger_definition(*contents, handle);
}

Result<std::optional<LoggerSelection>> LoggerDefinitionService::load_selection(
    std::string_view conf_handle, std::string_view ecu_id, bytes::Bytes& conf_out)
{
    auto contents = repository_.read(conf_handle);
    if (!contents)
    {
        return std::unexpected(contents.error());
    }
    conf_out = std::move(*contents);
    return read_selection(conf_out, ecu_id, conf_handle);
}

Result<std::optional<LoggerSelection>> LoggerDefinitionService::load_selection(
    std::string_view conf_handle, std::string_view ecu_id)
{
    bytes::Bytes conf;
    return load_selection(conf_handle, ecu_id, conf);
}

Result<LoggerSelection> LoggerDefinitionService::load_or_initialize_selection(
    std::string_view conf_handle,
    std::string_view ecu_id,
    const LoggerDefinition& definition)
{
    bytes::Bytes contents;
    auto stored = load_selection(conf_handle, ecu_id, contents);
    if (!stored)
    {
        return std::unexpected(stored.error());
    }
    if (stored->has_value())
    {
        return std::move(**stored);
    }

    LoggerSelection selection = default_selection(definition);
    auto written = write_selection(contents, ecu_id, selection, conf_handle);
    if (!written)
    {
        return std::unexpected(written.error());
    }
    if (auto replaced = writer_.replace(conf_handle, *written); !replaced)
    {
        return std::unexpected(replaced.error());
    }
    return selection;
}

Status LoggerDefinitionService::save_selection(
    std::string_view conf_handle,
    std::string_view ecu_id,
    const LoggerSelection& selection)
{
    auto contents = repository_.read(conf_handle);
    if (!contents)
    {
        return std::unexpected(contents.error());
    }
    auto written = write_selection(*contents, ecu_id, selection, conf_handle);
    if (!written)
    {
        return std::unexpected(written.error());
    }
    return writer_.replace(conf_handle, *written);
}

} // namespace fastecu::logging
