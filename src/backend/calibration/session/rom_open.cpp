#include "src/backend/calibration/session/rom_open.h"

#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "src/algorithms/memory/memory_map.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/backend/config/catalog.h"

namespace fastecu::calibration
{
namespace
{

// QFileInfo::fileName() on the '/'-separated paths Qt's file dialogs return,
// with legacy's fallback for a path that names no file.
std::string DisplayName(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return name.empty() ? std::string{"default.bin"} : std::string{name};
}

// Legacy: QString(flash_method).remove(0, 3).insert(0, "checksum").
std::string ChecksumModuleFor(const std::string& flash_method)
{
    return "checksum" + (flash_method.size() > 3 ? flash_method.substr(3) : std::string{});
}

std::string_view FormatName(definition::DefinitionFormat format)
{
    return format == definition::DefinitionFormat::kEcuFlash ? "EcuFlash" : "RomRaider";
}

} // namespace

RomOpenUseCase::RomOpenUseCase(IDefinitionCatalogs& catalogs, definition::DefinitionService& definitions,
                               IFileRepository& files, IFileSystem& file_system, IEventSink& events,
                               config::ConfigSession& config)
    : catalogs_(catalogs), definitions_(definitions), files_(files), file_system_(file_system), events_(events),
      config_(config)
{
}

Result<RomOpenOutcome> RomOpenUseCase::OpenFile(std::string_view path)
{
    if (path.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "open_file called with no filename");
    }
    Result<std::vector<std::uint8_t>> rom = ReadRom(path, files_);
    if (!rom.has_value())
    {
        LogError("Unable to open calibration file", rom.error());
        events_.Notice("Calibration file: Unable to open calibration file for reading");
        return std::unexpected(rom.error());
    }
    return Finish(Seed{
        .source = {.display_name = DisplayName(path), .path = std::string{path}, .origin = RomOrigin::kFile},
        .rom = std::move(*rom),
    });
}

Result<RomOpenOutcome> RomOpenUseCase::AdoptReadImage(ReadImage image)
{
    if (image.filename.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "adopt_read_image called with no filename");
    }
    if (image.rom.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "adopt_read_image called with an empty image");
    }
    // Fire-and-forget, as legacy: a failed backup must not fail the open.
    BackupRom(image.rom, config_.EffectivePaths().calibration_files_directory + "read.bin", files_);
    return Finish(Seed{
        .source = {.display_name = DisplayName(image.filename), .path = image.filename, .origin = RomOrigin::kEcuRead},
        .rom = std::move(image.rom),
        .rom_id = std::move(image.rom_id),
        .flash_method = std::move(image.protocol_name),
        .kernel_path = std::move(image.kernel_path),
        .kernel_start_address = std::move(image.kernel_start_address),
    });
}

Result<RomOpenOutcome> RomOpenUseCase::Finish(Seed seed)
{
    std::string rom_id = std::move(seed.rom_id);
    std::optional<ResolvedDefinition> definition = FindDefinition(seed.rom, rom_id);

    // A loaded definition replaces the seed's flash method and checksum
    // module, as legacy's wholesale RomInfo replacement did; only then is the
    // flash-method alias resolved.
    std::string flash_method = std::move(seed.flash_method);
    std::string checksum_module;
    if (definition.has_value())
    {
        flash_method = ResolveAlias(definition->definition.metadata.flash_method);
        checksum_module = definition->definition.metadata.checksum_module;
    }

    // Checked before selecting a vehicle, so a rejected file changes nothing.
    // A flash method naming no catalog protocol gets the identity map.
    const std::size_t file_size = seed.rom.size();
    auto image = config::PlaceRomFile(config_.FindProtocol(flash_method), std::move(seed.rom));
    if (!image.has_value())
    {
        const auto failed = Fail(ErrorKind::kInvalidConfig, image.error().detail);
        LogError("ROM file does not fit its protocol's memory map", failed.error());
        events_.Notice(std::format("File size error: {}", image.error().detail));
        return failed;
    }

    const bool vehicle_selected = config_.SelectByProtocolName(flash_method);
    const config::VehicleSpec *vehicle = config_.SelectedVehicle();
    if (vehicle != nullptr)
    {
        switch (vehicle->protocol->checksum)
        {
        case config::ChecksumSupport::kCorrected:
            checksum_module = ChecksumModuleFor(flash_method);
            break;
        case config::ChecksumSupport::kMissing:
            checksum_module = "Not implemented yet";
            break;
        case config::ChecksumSupport::kNone:
            checksum_module = "No checksums";
            break;
        }
    }

    return RomOpenOutcome{
        .contents =
            SessionContents{
                .source = std::move(seed.source),
                .image = std::move(*image),
                .definition = std::move(definition),
                .protocol =
                    RomProtocolInfo{
                        .flash_method = flash_method,
                        .checksum_module = std::move(checksum_module),
                        .mcu_type = vehicle != nullptr ? std::string(vehicle->protocol->mcu) : std::string{},
                        .kernel_path = std::move(seed.kernel_path),
                        .kernel_start_address = std::move(seed.kernel_start_address),
                        .rom_id = std::move(rom_id),
                        .file_size_label = std::format("{}kb", file_size / 1024),
                    },
            },
        .vehicle_selected = vehicle_selected,
    };
}

std::optional<ResolvedDefinition> RomOpenUseCase::FindDefinition(std::span<const std::uint8_t> rom, std::string& rom_id)
{
    using definition::DefinitionFormat;
    const config::AppConfig& settings = config_.Settings();
    const bool ecuflash_enabled = settings.use_ecuflash_definitions == "enabled";
    const bool romraider_enabled = settings.use_romraider_definitions == "enabled";

    std::optional<ResolvedDefinition> found;
    if ((settings.primary_definition_base == "ecuflash" || !romraider_enabled) &&
        !settings.ecuflash_definition_files_directory.empty())
    {
        if (ecuflash_enabled)
        {
            found = TryFormat(DefinitionFormat::kEcuFlash, rom, rom_id);
        }
        if (!found.has_value() && romraider_enabled)
        {
            found = TryFormat(DefinitionFormat::kRomRaider, rom, rom_id);
        }
    }
    else if (settings.primary_definition_base == "romraider" && !settings.romraider_definition_files.empty())
    {
        if (romraider_enabled)
        {
            found = TryFormat(DefinitionFormat::kRomRaider, rom, rom_id);
        }
        if (!found.has_value() && ecuflash_enabled)
        {
            found = TryFormat(DefinitionFormat::kEcuFlash, rom, rom_id);
        }
    }
    return found;
}

std::optional<ResolvedDefinition> RomOpenUseCase::TryFormat(definition::DefinitionFormat format,
                                                            std::span<const std::uint8_t> rom, std::string& rom_id)
{
    const std::string match_operation = std::format("Unable to match {} definition", FormatName(format));
    Result<definition::DefinitionCatalog> catalog = catalogs_.Catalog(format);
    if (!catalog.has_value())
    {
        LogError(match_operation, catalog.error());
        return std::nullopt;
    }

    Result<definition::DefinitionIndexEntry> match = definitions_.MatchRom(*catalog, rom, MemoryMapsFor(rom.size()));
    if (match.has_value())
    {
        rom_id = match->definition_id;
        events_.Log(LogLevel::kDebug, std::format("{} cal id {} found", FormatName(format), rom_id));
    }
    else
    {
        // Legacy keeps the previous ID -- for an ECU read, the one the ECU
        // reported -- and still tries to load it below.
        LogError(match_operation, match.error());
    }

    if (rom_id.empty())
    {
        return std::nullopt;
    }
    std::string source;
    Result<definition::RomDefinition> loaded = Fail(ErrorKind::kInvalidConfig, "definition is not in the catalog");
    if (auto entry = catalog->Find(format, rom_id); entry.has_value())
    {
        source = entry->get().source;
        loaded = definitions_.Load(*catalog, format, rom_id);
    }
    else
    {
        // The fresh catalog skips files that became unreadable after the
        // sources were indexed. Legacy loaded from those indexes, so such a
        // definition still reached the load failure and its notice.
        std::optional<std::string> indexed = catalogs_.IndexedSource(format, rom_id);
        if (!indexed.has_value())
        {
            return std::nullopt;
        }
        source = std::move(*indexed);
    }
    if (!loaded.has_value())
    {
        LogError(std::format("Unable to read {} definition {}", FormatName(format), rom_id), loaded.error());
        if (!source.empty() && !file_system_.Exists(source))
        {
            events_.Notice(
                std::format("Ecu definitions file: Unable to open ECU definition file {} for reading", source));
        }
        return std::nullopt;
    }
    return ResolvedDefinition{.format = format, .id = rom_id, .definition = std::move(*loaded)};
}

definition::MemoryMapLookup RomOpenUseCase::MemoryMapsFor(std::size_t file_size) const
{
    // Resolves the alias without ResolveAlias's log lines: those describe the
    // one definition chosen, not every candidate tried.
    return [this, file_size](std::string_view flash_method) -> std::optional<memory::MemoryMap>
    {
        const config::VehicleSpec *aliased = config_.VehicleForAlias(flash_method);
        const std::string_view name = aliased != nullptr ? aliased->protocol->name : flash_method;
        // A size the protocol has no map for still identifies the definition on
        // the identity map; Finish then rejects the file by name.
        auto map = config::SelectMemoryMap(config_.FindProtocol(name), file_size);
        if (!map.has_value())
        {
            map = config::SelectMemoryMap(nullptr, file_size);
        }
        return map.has_value() ? std::optional(std::move(*map)) : std::nullopt;
    };
}

std::string RomOpenUseCase::ResolveAlias(const std::string& flash_method)
{
    const config::VehicleSpec *vehicle = config_.VehicleForAlias(flash_method);
    if (vehicle == nullptr)
    {
        return flash_method;
    }
    events_.Log(LogLevel::kDebug, std::format("Alias: {}", flash_method));
    events_.Log(LogLevel::kDebug, std::format("Protocol: {}", vehicle->protocol->name));
    return std::string(vehicle->protocol->name);
}

void RomOpenUseCase::LogError(std::string_view operation, const Error& error)
{
    events_.Log(LogLevel::kError, std::format("{} [{}]: {}", operation, ToString(error.kind), error.detail));
}

} // namespace fastecu::calibration
