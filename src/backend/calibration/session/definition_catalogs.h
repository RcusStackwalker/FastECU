#pragma once

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
};

} // namespace fastecu::calibration
