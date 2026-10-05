#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fastecu::definition
{

// Prepare UTF-8 authoring XML for pugixml: resolve internal DTD entities,
// validate character references, and never fetch external entities.
// Invalid references, cycles or excessive expansion yield no document.
std::optional<std::string> prepare_definition_xml(std::string_view xml);

} // namespace fastecu::definition
