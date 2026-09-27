#pragma once
#include <string>
#include <string_view>

#include <pugixml.hpp>

#include "src/backend/config/config_paths.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::config
{

// Reads and parses paths.protocols_file, the one XML file both the
// <protocols> and the <car_models> catalogs load from. A repository read
// error is returned unchanged; unparseable XML is InvalidConfig.
Result<pugi::xml_document> load_protocols_document(const ConfigPaths& paths, IFileRepository& file_repository);

// The text content of `node`'s first <tag> child, or "" when there is none.
std::string text_or_empty(pugi::xml_node node, std::string_view tag);

} // namespace fastecu::config
