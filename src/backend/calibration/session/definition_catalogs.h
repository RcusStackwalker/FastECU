#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Where a ROM open finds definitions to match and load. The desktop's
// implementation builds catalogs from configured sources and definitions
// authored this session, retaining startup source provenance independently.
class IDefinitionCatalogs
{
  public:
    virtual ~IDefinitionCatalogs() = default;
    // Called at most once per format per open.
    virtual Result<definition::DefinitionCatalog> Catalog(definition::DefinitionFormat format) = 0;
    // The source file of definition `id` as it was indexed when the catalogs'
    // sources were last scanned, or nullopt if it never was. A fresh catalog
    // skips a file that has since become unreadable; this is how an open
    // still reports that file, as legacy did from its startup indexes.
    virtual std::optional<std::string> IndexedSource(definition::DefinitionFormat format, std::string_view id) = 0;
};

} // namespace fastecu::calibration
