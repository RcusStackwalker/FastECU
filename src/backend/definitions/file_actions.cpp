#include "src/backend/definitions/file_actions.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "src/algorithms/diagnostics/dtc_parser.h"
#include "src/algorithms/protocol/qt_compat/qt_bytes.h"
#include "src/algorithms/diagnostics/nrc_parser.h"

namespace
{

using fastecu::definition::DefinitionCatalog;
using fastecu::definition::DefinitionFormat;

// Project only the startup browse indexes; portable sessions retain the full catalog.
void populate_catalog(FileActions::DefinitionIndexes& indexes, const DefinitionCatalog& catalog,
                      DefinitionFormat format)
{
    auto next = indexes;
    const bool romraider = format == DefinitionFormat::RomRaider;
    auto& ids = romraider ? next.romraider_def_cal_id : next.ecuflash_def_cal_id;
    auto& addresses = romraider ? next.romraider_def_cal_id_addr : next.ecuflash_def_cal_id_addr;
    auto& ecuIds = romraider ? next.romraider_def_ecu_id : next.ecuflash_def_ecu_id;
    auto& sources = romraider ? next.romraider_def_filename : next.ecuflash_def_filename;
    ids.clear();
    addresses.clear();
    ecuIds.clear();
    sources.clear();
    for (const auto& entry : catalog.entries())
    {
        if (entry.format != format)
        {
            continue;
        }
        ids.append(QString::fromStdString(entry.definition_id));
        addresses.append(entry.internal_id_address.has_value()
                             ? QString::fromStdString(std::format("{:x}", *entry.internal_id_address))
                             : QString{});
        ecuIds.append(QString::fromStdString(entry.ecu_id));
        sources.append(QString::fromStdString(entry.source));
    }
    indexes = std::move(next);
}

} // namespace

FileActions::FileActions(fastecu::IFileSystem& file_system, fastecu::IResourceBundle& /*resource_bundle*/,
                         fastecu::IFileRepository& file_repository, fastecu::IAtomicFileWriter& atomic_file_writer,
                         fastecu::IEventSink& events, fastecu::config::ConfigSession& config)
    : configSession_(config), definitionFileSystem_(file_system),
      definitionService_(file_system, file_repository, atomic_file_writer), events_(events)
{
}

fastecu::Result<DefinitionCatalog> FileActions::build_definition_catalog(DefinitionFormat format)
{
    const fastecu::config::AppConfig& settings = configSession_.settings();
    if (format == DefinitionFormat::RomRaider)
    {
        return definitionService_.build_romraider_catalog(settings.romraider_definition_files);
    }
    return definitionService_.build_ecuflash_catalog(settings.ecuflash_definition_files_directory,
                                                     submittedEcuflashHandles_);
}

QString FileActions::definition_source(DefinitionFormat format, const QString& id) const
{
    const QStringList *ids = format == DefinitionFormat::RomRaider ? &definitionIndexes.romraider_def_cal_id
                                                                   : &definitionIndexes.ecuflash_def_cal_id;
    const QStringList *sources = format == DefinitionFormat::RomRaider ? &definitionIndexes.romraider_def_filename
                                                                       : &definitionIndexes.ecuflash_def_filename;
    const qsizetype index = ids->indexOf(id);
    return index >= 0 && index < sources->size() ? sources->at(index) : QString{};
}

void FileActions::log_definition_error(const QString& operation, const fastecu::Error& error)
{
    events_.log(fastecu::LogLevel::Error,
                std::format("{} [{}]: {}", operation.toStdString(), fastecu::to_string(error.kind), error.detail));
}

fastecu::Status FileActions::submit_new_definition(std::string_view destination,
                                                   const fastecu::definition::DefinitionHeaderInput& input)
{
    // The caller reaches this only after the native "Save As" dialog already asked to
    // overwrite an existing file, so that confirmation is passed through here rather than
    // rejected again by the service's own new-file-only guard.
    fastecu::Status status = definitionService_.create_definition(destination, input, /*allow_overwrite=*/true);
    if (!status.has_value())
    {
        log_definition_error("Unable to create definition", status.error());
    }
    else
    {
        remember_submitted_ecuflash_handle(destination);
    }
    return status;
}

fastecu::Status FileActions::submit_imported_definition(std::string_view source, std::string_view destination,
                                                        const fastecu::definition::DefinitionHeaderInput& input)
{
    fastecu::Status status = definitionService_.import_definition(source, destination, input);
    if (!status.has_value())
    {
        log_definition_error("Unable to import definition", status.error());
    }
    else
    {
        remember_submitted_ecuflash_handle(destination);
    }
    return status;
}

void FileActions::remember_submitted_ecuflash_handle(std::string_view destination)
{
    const auto position = std::ranges::lower_bound(submittedEcuflashHandles_, destination, {},
                                                   [](const auto& item) { return std::string_view{item}; });
    if (position == std::ranges::end(submittedEcuflashHandles_) || std::string_view(*position) != destination)
    {
        submittedEcuflashHandles_.emplace(position, destination);
    }
}

QString FileActions::parse_hex_ecuid(uint8_t byte)
{
    QString ecuid_byte;
    static constexpr std::string_view chars = "0123456789ABCDEF";

    const unsigned value = byte;
    ecuid_byte = (QChar)chars[(value >> 4U) & 0xFU];
    ecuid_byte.append((QChar)chars[value & 0xFU]);
    // emit LOG_D("Constructed byte: " + ecuid_byte;

    return ecuid_byte;
}

QString FileActions::parse_nrc_message(const QByteArray& nrc)
{
    return QString::fromStdString(nrc_description(bytes::view(nrc)));
}

QString FileActions::parse_dtc_message(uint16_t dtc)
{
    return QString::fromStdString(dtc_description(dtc));
}

void FileActions::create_ecuflash_def_id_list()
{
    const std::string& directory = configSession_.settings().ecuflash_definition_files_directory;
    if (directory.empty())
    {
        events_.log(fastecu::LogLevel::Debug, "No EcuFlash definition files directory");
        return;
    }

    auto replaced = definitionService_.build_ecuflash_catalog(directory, submittedEcuflashHandles_);
    if (!replaced.has_value())
    {
        log_definition_error("Unable to build EcuFlash definition catalog", replaced.error());
        return;
    }

    populate_catalog(definitionIndexes, *replaced, DefinitionFormat::EcuFlash);
    std::set<QString> sources;
    for (const QString& source : definitionIndexes.ecuflash_def_filename)
    {
        sources.insert(source);
    }
    events_.log(fastecu::LogLevel::Debug, std::format("{} EcuFlash definition files found", sources.size()));
    events_.log(fastecu::LogLevel::Debug,
                std::format("{} EcuFlash ecu id's found", definitionIndexes.ecuflash_def_cal_id.size()));
}

void FileActions::create_romraider_def_id_list()
{
    const std::vector<std::string>& handles = configSession_.settings().romraider_definition_files;
    if (handles.empty())
    {
        events_.log(fastecu::LogLevel::Debug, "No RomRaider definition files");
        return;
    }

    for (const std::string& handle : handles)
    {
        events_.log(fastecu::LogLevel::Debug, std::format("Reading RomRaider ID's from file: {}", handle));
    }

    auto replaced = definitionService_.build_romraider_catalog(handles);
    if (!replaced.has_value())
    {
        log_definition_error("Unable to build RomRaider definition catalog", replaced.error());
        for (const std::string& handle : handles)
        {
            if (!definitionFileSystem_.exists(handle))
            {
                events_.notice(std::format(
                    "Ecu definition file: Unable to open romraider definition file {} for reading", handle));
                break;
            }
        }
        return;
    }

    populate_catalog(definitionIndexes, *replaced, DefinitionFormat::RomRaider);
    events_.log(fastecu::LogLevel::Debug, std::format("{} RomRaider definition files found", handles.size()));
    events_.log(fastecu::LogLevel::Debug,
                std::format("{} RomRaider ecu id's found", definitionIndexes.romraider_def_cal_id.size()));
}
