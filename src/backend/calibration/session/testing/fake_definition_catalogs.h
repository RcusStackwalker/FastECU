#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/calibration/session/definition_catalogs.h"

namespace fastecu::calibration::testing
{

class FakeDefinitionCatalogs : public IDefinitionCatalogs
{
  public:
    Result<definition::DefinitionCatalog> catalog(definition::DefinitionFormat format) override
    {
        calls.push_back(format);
        if (auto error = errors.find(format); error != errors.end())
        {
            return std::unexpected(error->second);
        }
        auto found = entries.find(format);
        return definition::DefinitionCatalog::create(
            found == entries.end() ? std::vector<definition::DefinitionIndexEntry>{} : found->second);
    }

    std::optional<std::string> indexed_source(definition::DefinitionFormat format, std::string_view id) override
    {
        auto found = indexed_sources.find({format, std::string{id}});
        return found == indexed_sources.end() ? std::nullopt : std::optional<std::string>{found->second};
    }

    std::map<definition::DefinitionFormat, std::vector<definition::DefinitionIndexEntry>> entries;
    std::map<std::pair<definition::DefinitionFormat, std::string>, std::string> indexed_sources;
    std::map<definition::DefinitionFormat, Error> errors;
    std::vector<definition::DefinitionFormat> calls;
};

} // namespace fastecu::calibration::testing
