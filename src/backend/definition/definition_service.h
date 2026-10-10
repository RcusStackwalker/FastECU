#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "src/algorithms/memory/memory_map.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/definition/definition_writer.h"
#include "src/backend/ports/atomic_file_writer.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"

namespace fastecu::definition
{

// The memory map a ROM file takes under `flash_method` (a definition's
// effective flash method, before alias resolution), or nullopt when that
// definition cannot place the file at all.
using MemoryMapLookup = std::function<std::optional<memory::MemoryMap>(std::string_view flash_method)>;

class DefinitionService
{
  public:
    DefinitionService(IFileSystem&, IFileRepository&, IAtomicFileWriter&);

    // skip_unusable_handles controls whether a handle that fails to read or parse is skipped
    // (appropriate when scanning many files for a browsable catalog -- one bad file shouldn't
    // hide every other one) or fails the whole call (appropriate when every handle passed in is
    // a specific, required file, such as loading the one base definition a ROM's identity names).
    Result<DefinitionCatalog> BuildRomraiderCatalog(std::span<const std::string> ordered_handles,
                                                    bool skip_unusable_handles = true);
    Result<DefinitionCatalog> BuildEcuflashCatalog(std::string_view directory,
                                                   std::span<const std::string> explicit_handles = {},
                                                   bool skip_unusable_handles = true);
    // The first entry whose internal ID is the ROM file's bytes at the ECU
    // address its internal ID address stands for, through the memory map
    // `memory_maps` gives for the entry's own or inherited flash method.
    Result<DefinitionIndexEntry> MatchRom(const DefinitionCatalog&, std::span<const std::uint8_t> rom,
                                          const MemoryMapLookup& memory_maps) const;
    Result<RomDefinition> Load(const DefinitionCatalog&, DefinitionFormat, std::string_view id);
    Status CreateDefinition(std::string_view destination, const DefinitionHeaderInput&, bool allow_overwrite = false);
    Status ImportDefinition(std::string_view source, std::string_view destination, const DefinitionHeaderInput&);

  private:
    IFileSystem& file_system_;
    IFileRepository& repository_;
    IAtomicFileWriter& writer_;
};

} // namespace fastecu::definition
