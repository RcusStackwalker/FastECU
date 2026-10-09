#include "src/backend/definition/definition_catalog_session.h"
#include "src/backend/definition/text_format.h"

#include <algorithm>
#include <format>
#include <set>
#include <utility>

namespace fastecu::definition
{

DefinitionCatalogSession::DefinitionCatalogSession(fastecu::definition::DefinitionService& definitions,
                                                   config::ConfigSession& config, IFileSystem& file_system,
                                                   IEventSink& events)
    : definitions_(definitions), config_(config), file_system_(file_system), events_(events)
{
}

Result<DefinitionCatalog> DefinitionCatalogSession::Catalog(DefinitionFormat format)
{
    const auto& settings = config_.Settings();
    if (format == DefinitionFormat::kRomRaider)
    {
        return definitions_.BuildRomraiderCatalog(settings.romraider_definition_files);
    }
    return definitions_.BuildEcuflashCatalog(settings.ecuflash_definition_files_directory, submitted_ecuflash_handles_);
}

std::vector<DefinitionCatalogSession::IndexEntry>& DefinitionCatalogSession::Index(DefinitionFormat format)
{
    return format == DefinitionFormat::kRomRaider ? romraider_index_ : ecuflash_index_;
}

std::optional<std::string> DefinitionCatalogSession::IndexedSource(DefinitionFormat format, std::string_view id)
{
    const auto& records = Index(format);
    if (auto found = std::ranges::find(records, id, &IndexEntry::definition_id);
        found != records.end() && !found->source.empty())
    {
        return found->source;
    }
    return std::nullopt;
}

void DefinitionCatalogSession::LogError(std::string_view operation, const Error& error)
{
    events_.Log(LogLevel::kError, std::format("{} [{}]: {}", operation, ToString(error.kind), error.detail));
}

Status DefinitionCatalogSession::RefreshIndex(DefinitionFormat format)
{
    const auto& settings = config_.Settings();
    const bool romraider = format == DefinitionFormat::kRomRaider;
    const std::string_view name = romraider ? "RomRaider" : "EcuFlash";
    if (romraider ? settings.romraider_definition_files.empty() : settings.ecuflash_definition_files_directory.empty())
    {
        events_.Log(LogLevel::kDebug,
                    romraider ? "No RomRaider definition files" : "No EcuFlash definition files directory");
        return {};
    }
    if (romraider)
    {
        for (const auto& handle : settings.romraider_definition_files)
        {
            events_.Log(LogLevel::kDebug, std::format("Reading RomRaider ID's from file: {}", handle));
        }
    }

    auto scanned = Catalog(format);
    if (!scanned.has_value())
    {
        LogError(std::format("Unable to build {} definition catalog", name), scanned.error());
        if (romraider)
        {
            for (const auto& handle : settings.romraider_definition_files)
            {
                if (!file_system_.Exists(handle))
                {
                    events_.Notice(std::format(
                        "Ecu definition file: Unable to open romraider definition file {} for reading", handle));
                    break;
                }
            }
        }
        return std::unexpected(scanned.error());
    }

    std::vector<IndexEntry> replacement;
    std::set<std::string> sources;
    for (const auto& entry : scanned->Entries())
    {
        if (entry.format == format)
        {
            replacement.push_back({entry.definition_id, entry.source});
            sources.insert(entry.source);
        }
    }
    Index(format) = std::move(replacement);
    events_.Log(LogLevel::kDebug,
                std::format("{} {} definition files found",
                            romraider ? settings.romraider_definition_files.size() : sources.size(), name));
    events_.Log(LogLevel::kDebug, std::format("{} {} ecu id's found", Index(format).size(), name));
    return {};
}

void DefinitionCatalogSession::RememberSubmission(std::string_view destination, std::string_view id)
{
    id = TrimHeaderText(id);
    const auto position = std::ranges::lower_bound(submitted_ecuflash_handles_, destination);
    if (position == submitted_ecuflash_handles_.end() || *position != destination)
    {
        submitted_ecuflash_handles_.emplace(position, destination);
    }
    ecuflash_index_.push_back({std::string{id}, std::string{destination}});
}

Status DefinitionCatalogSession::SubmitNewDefinition(std::string_view destination,
                                                     const fastecu::definition::DefinitionHeaderInput& input,
                                                     bool allow_overwrite)
{
    Status status = definitions_.CreateDefinition(destination, input, allow_overwrite);
    if (!status.has_value())
    {
        LogError("Unable to create definition", status.error());
    }
    else
    {
        RememberSubmission(destination, input.xml_id);
    }
    return status;
}

Status DefinitionCatalogSession::SubmitImportedDefinition(std::string_view source, std::string_view destination,
                                                          const fastecu::definition::DefinitionHeaderInput& input)
{
    Status status = definitions_.ImportDefinition(source, destination, input);
    if (!status.has_value())
    {
        LogError("Unable to import definition", status.error());
    }
    else
    {
        RememberSubmission(destination, input.xml_id);
    }
    return status;
}

} // namespace fastecu::definition
