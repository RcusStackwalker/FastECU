#pragma once

#include <map>
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

    std::map<definition::DefinitionFormat, std::vector<definition::DefinitionIndexEntry>> entries;
    std::map<definition::DefinitionFormat, Error> errors;
    std::vector<definition::DefinitionFormat> calls;
};

} // namespace fastecu::calibration::testing
