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
        std::string_view directory);
    Status replace_definition(
        definitions::EcuCalDefStructure&,
        const DefinitionCatalog&,
        DefinitionFormat,
        std::string_view id);
    Status create_definition(
        std::string_view destination,
        const DefinitionHeaderInput&);
    Status import_definition(
        std::string_view source,
        std::string_view destination,
        const DefinitionHeaderInput&);

  private:
    DefinitionService& service_;
};

} // namespace fastecu::definition
