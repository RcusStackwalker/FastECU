#pragma once

#include <span>
#include <string>
#include <string_view>

#include "src/backend/definition/definition_service.h"
#include "src/backend/definitions/config_values.h"
#include "src/backend/definitions/ecu_cal_def.h"

namespace fastecu::definition
{

class LegacyDefinitionAdapter
{
  public:
    explicit LegacyDefinitionAdapter(DefinitionService&);

    Status replace_romraider_catalog(
        definitions::ConfigValuesStructure&,
        std::span<const std::string> ordered_handles);
    Status replace_ecuflash_catalog(
        definitions::ConfigValuesStructure&,
        std::string_view directory,
        std::span<const std::string> explicit_handles = {});
    // resolved, when non-null, receives the RomDefinition this call loaded --
    // only on success. It lets a caller reuse the resolution instead of
    // rebuilding the catalog and loading the same definition again.
    Status replace_definition(
        definitions::EcuCalDefStructure&,
        const DefinitionCatalog&,
        DefinitionFormat,
        std::string_view id,
        RomDefinition *resolved = nullptr);
    Status create_definition(
        std::string_view destination,
        const DefinitionHeaderInput&,
        bool allow_overwrite = false);
    Status import_definition(
        std::string_view source,
        std::string_view destination,
        const DefinitionHeaderInput&);

  private:
    DefinitionService& service_;
};

} // namespace fastecu::definition
