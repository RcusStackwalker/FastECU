#include "src/backend/config/protocols_document.h"

#include <cstdint>
#include <format>
#include <vector>

namespace fastecu::config
{

Result<pugi::xml_document> load_protocols_document(const ConfigPaths& paths, IFileRepository& file_repository)
{
    Result<std::vector<std::uint8_t>> bytes = file_repository.read(paths.protocols_file);
    if (!bytes.has_value())
    {
        return std::unexpected(bytes.error());
    }

    pugi::xml_document doc;
    if (pugi::xml_parse_result parsed = doc.load_buffer(bytes->data(), bytes->size()); !parsed)
    {
        return fail(ErrorKind::InvalidConfig, std::format("protocols parse error: {}", parsed.description()));
    }
    return doc;
}

std::string text_or_empty(pugi::xml_node node, std::string_view tag)
{
    return node.child(tag).text().as_string();
}

} // namespace fastecu::config
