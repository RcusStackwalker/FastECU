#pragma once

#include <functional>
#include <string_view>

#include "src/backend/definition/definition_model.h"

namespace fastecu::definition
{

using DefinitionLoader =
    std::function<Result<UnresolvedDefinition>(DefinitionFormat, std::string_view id)>;

Result<RomDefinition> resolve_definition(
    UnresolvedDefinition root, const DefinitionLoader& loader);

} // namespace fastecu::definition
