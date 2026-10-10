#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/definition_catalogs.h"
#include "src/backend/config/config_session.h"
#include "src/backend/definition/definition_service.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// An image just read off an ECU, handed over only after the read succeeded.
struct ReadImage
{
    std::vector<std::uint8_t> rom;
    // Required. The caller names it (flash::read_image_filename), as
    // MainWindow already does before every legacy open.
    std::string filename;
    // As reported by the read; a definition match replaces it.
    std::string rom_id;
    // The protocol selected when the read ran.
    std::string protocol_name;
    std::string kernel_path;
    std::string kernel_start_address;
};

struct RomOpenOutcome
{
    SessionContents contents;
    // ConfigSession::select_by_protocol_name found the ROM's flash method, so
    // the selected vehicle may have changed. The UI refreshes its protocol
    // display when this is set.
    bool vehicle_selected{false};
};

// FileActions::open_subaru_rom_file on typed values. Reading the image, an
// empty image, and a file size its protocol declares no memory map for fail
// the open; every later problem degrades to a definition-less session or a map
// that fails to decode, with the legacy log lines and notices.
class RomOpenUseCase
{
  public:
    RomOpenUseCase(IDefinitionCatalogs& catalogs, definition::DefinitionService& definitions, IFileRepository& files,
                   IFileSystem& file_system, IEventSink& events, config::ConfigSession& config);

    Result<RomOpenOutcome> OpenFile(std::string_view path);
    Result<RomOpenOutcome> AdoptReadImage(ReadImage image);

  private:
    struct Seed
    {
        RomSource source;
        std::vector<std::uint8_t> rom;
        std::string rom_id;
        std::string flash_method;
        std::string kernel_path;
        std::string kernel_start_address;
    };

    Result<RomOpenOutcome> Finish(Seed seed);
    std::optional<ResolvedDefinition> FindDefinition(std::span<const std::uint8_t> rom, std::string& rom_id);
    std::optional<ResolvedDefinition> TryFormat(definition::DefinitionFormat format, std::span<const std::uint8_t> rom,
                                                std::string& rom_id);
    // Each definition's memory map for a file of `file_size` bytes: its flash
    // method's protocol's map, or the identity map when that protocol declares
    // none for this size (Finish then rejects the file by name) or names no
    // catalog protocol.
    definition::MemoryMapLookup MemoryMapsFor(std::size_t file_size) const;
    std::string ResolveAlias(const std::string& flash_method);
    void LogError(std::string_view operation, const Error& error);

    IDefinitionCatalogs& catalogs_;
    definition::DefinitionService& definitions_;
    IFileRepository& files_;
    IFileSystem& file_system_;
    IEventSink& events_;
    config::ConfigSession& config_;
};

} // namespace fastecu::calibration
