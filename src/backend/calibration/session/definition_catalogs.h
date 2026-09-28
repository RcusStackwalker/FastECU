#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Where a ROM open finds definitions to match and load. The desktop's
// implementation is FileActions, which builds catalogs from the configured
// sources and the definitions authored this session; step 6n replaces it.
class IDefinitionCatalogs
{
  public:
    virtual ~IDefinitionCatalogs() = default;
    // Called at most once per format per open.
    virtual Result<definition::DefinitionCatalog> catalog(definition::DefinitionFormat format) = 0;
    // The source file of definition `id` as it was indexed when the catalogs'
    // sources were last scanned, or nullopt if it never was. A fresh catalog
    // skips a file that has since become unreadable; this is how an open
    // still reports that file, as legacy did from its startup indexes.
    virtual std::optional<std::string> indexed_source(definition::DefinitionFormat format, std::string_view id) = 0;
};

} // namespace fastecu::calibration
