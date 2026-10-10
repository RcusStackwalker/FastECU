#pragma once

#include <cstddef>
#include <functional>
#include <string_view>

#include "src/backend/definition/definition_model.h"

namespace fastecu::definition
{

// Real definition families are at most a handful of levels deep; this is a generous ceiling
// that still fails fast with a resolvable error well short of the C++ call stack limit for
// the resolver's recursion. Matching walks inheritance under the same ceiling.
inline constexpr std::size_t kMaxInheritanceDepth = 256;

using DefinitionLoader = std::function<Result<UnresolvedDefinition>(DefinitionFormat, std::string_view id)>;

Result<RomDefinition> ResolveDefinition(UnresolvedDefinition root, const DefinitionLoader& loader);

} // namespace fastecu::definition
