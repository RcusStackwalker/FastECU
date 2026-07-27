#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "src/backend/definition/definition_model.h"
#include "src/backend/definition/definition_writer.h"
#include "src/backend/ports/atomic_file_writer.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"

namespace fastecu::definition
{

class DefinitionService
{
  public:
    DefinitionService(IFileSystem&, IFileRepository&, IAtomicFileWriter&);

    Result<DefinitionCatalog> build_romraider_catalog(
        std::span<const std::string> ordered_handles);
    Result<DefinitionCatalog> build_ecuflash_catalog(std::string_view directory);
    Result<DefinitionIndexEntry> match_rom(
        const DefinitionCatalog&, std::span<const std::uint8_t> rom) const;
    Result<RomDefinition> load(
        const DefinitionCatalog&, DefinitionFormat, std::string_view id);
    Status create_definition(
        std::string_view destination,
        const DefinitionHeaderInput&);
    Status import_definition(
        std::string_view source,
        std::string_view destination,
        const DefinitionHeaderInput&);

  private:
    IFileSystem& file_system_;
    IFileRepository& repository_;
    IAtomicFileWriter& writer_;
};

} // namespace fastecu::definition
