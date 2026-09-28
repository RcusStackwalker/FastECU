#include "src/backend/calibration/session/rom_open.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string_view>
#include <utility>

#include "src/backend/calibration/calibration_service.h"

namespace fastecu::calibration
{
namespace
{

// QFileInfo::fileName() on the '/'-separated paths Qt's file dialogs return,
// with legacy's fallback for a path that names no file.
std::string display_name(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return name.empty() ? std::string{"default.bin"} : std::string{name};
}

// Legacy: QString(flash_method).remove(0, 3).insert(0, "checksum").
std::string checksum_module_for(const std::string& flash_method)
{
    return "checksum" + (flash_method.size() > 3 ? flash_method.substr(3) : std::string{});
}

std::string_view format_name(definition::DefinitionFormat format)
{
    return format == definition::DefinitionFormat::EcuFlash ? "EcuFlash" : "RomRaider";
}

// Legacy: QString(alias).split(",").contains(flash_method).
bool alias_list_contains(std::string_view aliases, std::string_view flash_method)
{
    return std::ranges::any_of(std::views::split(aliases, ','), [flash_method](const auto part)
                               { return std::string_view{part.begin(), part.end()} == flash_method; });
}

} // namespace

RomOpenUseCase::RomOpenUseCase(IDefinitionCatalogs& catalogs, definition::DefinitionService& definitions,
                               IFileRepository& files, IFileSystem& file_system, IEventSink& events,
                               config::ConfigSession& config)
    : catalogs_(catalogs), definitions_(definitions), files_(files), file_system_(file_system), events_(events),
      config_(config)
{
}

Result<RomOpenOutcome> RomOpenUseCase::open_file(std::string_view path)
{
    if (path.empty())
    {
        return fail(ErrorKind::InvalidConfig, "open_file called with no filename");
    }
    Result<std::vector<std::uint8_t>> rom = read_rom(path, files_);
    if (!rom.has_value())
    {
        log_error("Unable to open calibration file", rom.error());
        events_.notice("Calibration file: Unable to open calibration file for reading");
        return std::unexpected(rom.error());
    }
    return finish(Seed{
        .source = {.display_name = display_name(path), .path = std::string{path}, .origin = RomOrigin::File},
        .rom = std::move(*rom),
    });
}

Result<RomOpenOutcome> RomOpenUseCase::adopt_read_image(ReadImage image)
{
    if (image.filename.empty())
    {
        return fail(ErrorKind::InvalidConfig, "adopt_read_image called with no filename");
    }
    if (image.rom.empty())
    {
        return fail(ErrorKind::InvalidConfig, "adopt_read_image called with an empty image");
    }
    // Fire-and-forget, as legacy: a failed backup must not fail the open.
    backup_rom(image.rom, config_.effective_paths().calibration_files_directory + "read.bin", files_);
    return finish(Seed{
        .source = {.display_name = display_name(image.filename), .path = image.filename, .origin = RomOrigin::EcuRead},
        .rom = std::move(image.rom),
        .rom_id = std::move(image.rom_id),
        .flash_method = std::move(image.protocol_name),
        .kernel_path = std::move(image.kernel_path),
        .kernel_start_address = std::move(image.kernel_start_address),
    });
}

RomOpenOutcome RomOpenUseCase::finish(Seed seed)
{
    RomOpenOutcome outcome;
    std::string rom_id = std::move(seed.rom_id);
    std::optional<ResolvedDefinition> definition = find_definition(seed.rom, rom_id);

    // A loaded definition replaces the seed's flash method and checksum
    // module, as legacy's wholesale RomInfo replacement did; only then is the
    // flash-method alias resolved.
    std::string flash_method = std::move(seed.flash_method);
    std::string checksum_module;
    if (definition.has_value())
    {
        flash_method = resolve_alias(definition->definition.metadata.flash_method);
        checksum_module = definition->definition.metadata.checksum_module;
    }

    outcome.vehicle_selected = config_.select_by_protocol_name(flash_method);
    const config::ResolvedCarModel *vehicle = config_.selected_vehicle();
    const std::string selected_checksum =
        vehicle != nullptr ? config::protocol_field_or_placeholder(*vehicle, &config::ProtocolEntry::checksum)
                           : std::string{};
    if (selected_checksum == "yes")
    {
        checksum_module = checksum_module_for(flash_method);
    }
    else if (selected_checksum == "n/a")
    {
        checksum_module = "Not implemented yet";
    }
    else if (selected_checksum == "no")
    {
        checksum_module = "No checksums";
    }

    const std::size_t unpadded_size = seed.rom.size();
    std::vector<std::uint8_t> rom = apply_flash_method_padding(std::move(seed.rom), flash_method);

    if (definition.has_value())
    {
        const Status size_ok = validate_rom_size(definition->definition, rom.size());
        if (!size_ok.has_value())
        {
            log_error("Error in expected ROM size", size_ok.error());
            events_.notice("File size error: Error in expected ROM size!");
            definition->definition.maps.clear();
            outcome.size_rejected = true;
        }
    }

    outcome.contents = SessionContents{
        .source = std::move(seed.source),
        .rom = std::move(rom),
        .definition = std::move(definition),
        .protocol =
            RomProtocolInfo{
                .flash_method = flash_method,
                .checksum_module = std::move(checksum_module),
                .mcu_type = vehicle != nullptr
                                ? config::protocol_field_or_placeholder(*vehicle, &config::ProtocolEntry::mcu)
                                : std::string{},
                .kernel_path = std::move(seed.kernel_path),
                .kernel_start_address = std::move(seed.kernel_start_address),
                .rom_id = std::move(rom_id),
                .file_size_label = std::format("{}kb", unpadded_size / 1024),
                .unpadded_size = unpadded_size,
            },
    };
    return outcome;
}

std::optional<ResolvedDefinition> RomOpenUseCase::find_definition(std::span<const std::uint8_t> rom,
                                                                  std::string& rom_id)
{
    using definition::DefinitionFormat;
    const config::AppConfig& settings = config_.settings();
    const bool ecuflash_enabled = settings.use_ecuflash_definitions == "enabled";
    const bool romraider_enabled = settings.use_romraider_definitions == "enabled";

    std::optional<ResolvedDefinition> found;
    if ((settings.primary_definition_base == "ecuflash" || !romraider_enabled) &&
        !settings.ecuflash_definition_files_directory.empty())
    {
        if (ecuflash_enabled)
        {
            found = try_format(DefinitionFormat::EcuFlash, rom, rom_id);
        }
        if (!found.has_value() && romraider_enabled)
        {
            found = try_format(DefinitionFormat::RomRaider, rom, rom_id);
        }
    }
    else if (settings.primary_definition_base == "romraider" && !settings.romraider_definition_files.empty())
    {
        if (romraider_enabled)
        {
            found = try_format(DefinitionFormat::RomRaider, rom, rom_id);
        }
        if (!found.has_value() && ecuflash_enabled)
        {
            found = try_format(DefinitionFormat::EcuFlash, rom, rom_id);
        }
    }
    return found;
}

std::optional<ResolvedDefinition> RomOpenUseCase::try_format(definition::DefinitionFormat format,
                                                             std::span<const std::uint8_t> rom, std::string& rom_id)
{
    const std::string match_operation = std::format("Unable to match {} definition", format_name(format));
    Result<definition::DefinitionCatalog> catalog = catalogs_.catalog(format);
    if (!catalog.has_value())
    {
        log_error(match_operation, catalog.error());
        return std::nullopt;
    }

    Result<definition::DefinitionIndexEntry> match = definitions_.match_rom(*catalog, rom);
    if (match.has_value())
    {
        rom_id = match->definition_id;
        events_.log(LogLevel::Debug, std::format("{} cal id {} found", format_name(format), rom_id));
    }
    else
    {
        // Legacy keeps the previous ID -- for an ECU read, the one the ECU
        // reported -- and still tries to load it below.
        log_error(match_operation, match.error());
    }

    if (rom_id.empty())
    {
        return std::nullopt;
    }
    std::string source;
    Result<definition::RomDefinition> loaded = fail(ErrorKind::InvalidConfig, "definition is not in the catalog");
    if (auto entry = catalog->find(format, rom_id); entry.has_value())
    {
        source = entry->get().source;
        loaded = definitions_.load(*catalog, format, rom_id);
    }
    else
    {
        // The fresh catalog skips files that became unreadable after the
        // sources were indexed. Legacy loaded from those indexes, so such a
        // definition still reached the load failure and its notice.
        std::optional<std::string> indexed = catalogs_.indexed_source(format, rom_id);
        if (!indexed.has_value())
        {
            return std::nullopt;
        }
        source = std::move(*indexed);
    }
    if (!loaded.has_value())
    {
        log_error(std::format("Unable to read {} definition {}", format_name(format), rom_id), loaded.error());
        if (!source.empty() && !file_system_.exists(source))
        {
            events_.notice(
                std::format("Ecu definitions file: Unable to open ECU definition file {} for reading", source));
        }
        return std::nullopt;
    }
    return ResolvedDefinition{.format = format, .id = rom_id, .definition = std::move(*loaded)};
}

std::string RomOpenUseCase::resolve_alias(const std::string& flash_method)
{
    for (const config::ResolvedCarModel& vehicle : config_.vehicles())
    {
        const std::string aliases = config::protocol_field_or_placeholder(vehicle, &config::ProtocolEntry::alias);
        if (alias_list_contains(aliases, flash_method))
        {
            events_.log(LogLevel::Debug, std::format("Alias: {}", flash_method));
            events_.log(LogLevel::Debug, std::format("Protocol: {}", vehicle.protocol_name));
            return vehicle.protocol_name;
        }
    }
    return flash_method;
}

void RomOpenUseCase::log_error(std::string_view operation, const Error& error)
{
    events_.log(LogLevel::Error, std::format("{} [{}]: {}", operation, to_string(error.kind), error.detail));
}

} // namespace fastecu::calibration
