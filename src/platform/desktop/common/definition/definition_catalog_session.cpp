#include "src/platform/desktop/common/definition/definition_catalog_session.h"

#include <algorithm>
#include <format>
#include <set>
#include <utility>

namespace fastecu::desktop::definition
{
using fastecu::definition::DefinitionCatalog;
using fastecu::definition::DefinitionFormat;

DefinitionCatalogSession::DefinitionCatalogSession(fastecu::definition::DefinitionService& definitions,
                                                   config::ConfigSession& config, IFileSystem& file_system,
                                                   IEventSink& events)
    : definitions_(definitions), config_(config), file_system_(file_system), events_(events)
{
}

Result<DefinitionCatalog> DefinitionCatalogSession::catalog(DefinitionFormat format)
{
    const auto& settings = config_.settings();
    if (format == DefinitionFormat::RomRaider)
    {
        return definitions_.build_romraider_catalog(settings.romraider_definition_files);
    }
    return definitions_.build_ecuflash_catalog(settings.ecuflash_definition_files_directory,
                                               submitted_ecuflash_handles_);
}

std::vector<DefinitionCatalogSession::IndexedSource>& DefinitionCatalogSession::index(DefinitionFormat format)
{
    return format == DefinitionFormat::RomRaider ? romraider_index_ : ecuflash_index_;
}

std::optional<std::string> DefinitionCatalogSession::indexed_source(DefinitionFormat format, std::string_view id)
{
    const auto& records = index(format);
    if (auto found = std::ranges::find(records, id, &IndexedSource::definition_id);
        found != records.end() && !found->source.empty())
    {
        return found->source;
    }
    return std::nullopt;
}

void DefinitionCatalogSession::log_error(std::string_view operation, const Error& error)
{
    events_.log(LogLevel::Error, std::format("{} [{}]: {}", operation, to_string(error.kind), error.detail));
}

Status DefinitionCatalogSession::refresh_index(DefinitionFormat format)
{
    const auto& settings = config_.settings();
    const bool romraider = format == DefinitionFormat::RomRaider;
    const std::string_view name = romraider ? "RomRaider" : "EcuFlash";
    if (romraider ? settings.romraider_definition_files.empty() : settings.ecuflash_definition_files_directory.empty())
    {
        events_.log(LogLevel::Debug,
                    romraider ? "No RomRaider definition files" : "No EcuFlash definition files directory");
        return {};
    }
    if (romraider)
    {
        for (const auto& handle : settings.romraider_definition_files)
        {
            events_.log(LogLevel::Debug, std::format("Reading RomRaider ID's from file: {}", handle));
        }
    }

    auto scanned = catalog(format);
    if (!scanned.has_value())
    {
        log_error(std::format("Unable to build {} definition catalog", name), scanned.error());
        if (romraider)
        {
            for (const auto& handle : settings.romraider_definition_files)
            {
                if (!file_system_.exists(handle))
                {
                    events_.notice(std::format(
                        "Ecu definition file: Unable to open romraider definition file {} for reading", handle));
                    break;
                }
            }
        }
        return std::unexpected(scanned.error());
    }

    std::vector<IndexedSource> replacement;
    std::set<std::string> sources;
    for (const auto& entry : scanned->entries())
    {
        if (entry.format == format)
        {
            replacement.push_back({entry.definition_id, entry.source});
            sources.insert(entry.source);
        }
    }
    index(format) = std::move(replacement);
    events_.log(LogLevel::Debug,
                std::format("{} {} definition files found",
                            romraider ? settings.romraider_definition_files.size() : sources.size(), name));
    events_.log(LogLevel::Debug, std::format("{} {} ecu id's found", index(format).size(), name));
    return {};
}

void DefinitionCatalogSession::remember_submission(std::string_view destination, std::string_view id)
{
    const auto position = std::ranges::lower_bound(submitted_ecuflash_handles_, destination);
    if (position == submitted_ecuflash_handles_.end() || *position != destination)
    {
        submitted_ecuflash_handles_.emplace(position, destination);
    }
    ecuflash_index_.push_back({std::string{id}, std::string{destination}});
}

Status DefinitionCatalogSession::submit_new_definition(std::string_view destination,
                                                       const fastecu::definition::DefinitionHeaderInput& input,
                                                       bool allow_overwrite)
{
    Status status = definitions_.create_definition(destination, input, allow_overwrite);
    if (!status.has_value())
    {
        log_error("Unable to create definition", status.error());
    }
    else
    {
        remember_submission(destination, input.xml_id);
    }
    return status;
}

Status DefinitionCatalogSession::submit_imported_definition(std::string_view source, std::string_view destination,
                                                            const fastecu::definition::DefinitionHeaderInput& input)
{
    Status status = definitions_.import_definition(source, destination, input);
    if (!status.has_value())
    {
        log_error("Unable to import definition", status.error());
    }
    else
    {
        remember_submission(destination, input.xml_id);
    }
    return status;
}

} // namespace fastecu::desktop::definition
