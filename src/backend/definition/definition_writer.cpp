#include "src/backend/definition/definition_writer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/definition/text_format.h"
#include "src/backend/definition/metadata_fields.h"

using namespace std::literals::string_view_literals;

namespace fastecu::definition
{
namespace
{

Status validate_input(const DefinitionHeaderInput& input)
{
    if (input.xml_id.empty())
    {
        return fail(ErrorKind::InvalidConfig, "definition XML ID is required");
    }
    if (input.internal_id.empty())
    {
        return fail(ErrorKind::InvalidConfig, "definition internal ID is required");
    }
    if (input.ecu_id.empty())
    {
        return fail(ErrorKind::InvalidConfig, "definition ECU ID is required");
    }
    return {};
}

void set_unique_text(pugi::xml_node parent, const char *name, std::string_view value)
{
    pugi::xml_node child = parent.child(name);
    if (!child)
    {
        child = parent.append_child(name);
    }
    // node.text() only tracks the first pcdata child; clear every existing child first so a
    // stale nested element or extra text node from the source XML can't survive alongside the
    // freshly written value (xml_text::set() would otherwise append a sibling instead of
    // replacing the element's content).
    for (pugi::xml_node grandchild = child.first_child(); grandchild;)
    {
        pugi::xml_node next = grandchild.next_sibling();
        child.remove_child(grandchild);
        grandchild = next;
    }
    child.text().set(value);
    for (pugi::xml_node duplicate = child.next_sibling(name); duplicate;)
    {
        pugi::xml_node next = duplicate.next_sibling(name);
        parent.remove_child(duplicate);
        duplicate = next;
    }
}

void set_optional_hex(pugi::xml_node parent, const char *name, std::optional<std::uint64_t> value)
{
    if (value)
    {
        set_unique_text(parent, name, hex_text(*value));
        return;
    }
    // No known address: remove rather than write a placeholder, so an unset optional never
    // materializes as a misleading "0x0" address (or clobbers a parent's real address once this
    // header is merged through inheritance).
    for (pugi::xml_node duplicate = parent.child(name); duplicate;)
    {
        pugi::xml_node next = duplicate.next_sibling(name);
        parent.remove_child(duplicate);
        duplicate = next;
    }
}

Status update_header(pugi::xml_node root, const DefinitionHeaderInput& raw_input)
{
    const auto input = normalize_header_input(raw_input);
    if (auto valid = validate_input(input); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    pugi::xml_node rom_id = root.child("romid");
    if (!rom_id)
    {
        rom_id = root.prepend_child("romid");
    }
    set_unique_text(rom_id, "xmlid", input.xml_id);
    set_optional_hex(rom_id, "internalidaddress", input.internal_id_address);
    set_unique_text(rom_id, "internalidstring", input.internal_id);
    set_unique_text(rom_id, "ecuid", input.ecu_id);
    for (const auto& field : kEditableMetadataFields)
    {
        set_unique_text(rom_id, std::string{field.xml_name}.c_str(), input.metadata.*field.member);
    }
    set_unique_text(rom_id, "filesize", input.metadata.file_size);
    set_unique_text(rom_id, "notes", input.metadata.notes);
    set_unique_text(root, "include", input.include);
    set_unique_text(root, "notes", input.notes);
    return {};
}

void normalize_declaration(pugi::xml_document& document)
{
    for (pugi::xml_node node = document.first_child(); node;)
    {
        pugi::xml_node next = node.next_sibling();
        if (node.type() == pugi::node_declaration)
        {
            document.remove_child(node);
        }
        node = next;
    }
    pugi::xml_node declaration = document.prepend_child(pugi::node_declaration);
    declaration.append_attribute("version") = "1.0";
    declaration.append_attribute("encoding") = "UTF-8";
}

Result<std::vector<std::uint8_t>> serialize_and_validate(pugi::xml_document& document)
{
    normalize_declaration(document);
    std::ostringstream output;
    document.save(output, "  ", pugi::format_default, pugi::encoding_utf8);
    std::string xml = std::move(output).str();
    if (xml.empty() || xml.back() != '\n')
    {
        xml.push_back('\n');
    }

    std::vector<std::uint8_t> result(xml.begin(), xml.end());
    if (auto parsed = parse_ecuflash_definition(result, "generated definition"); !parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    return result;
}

pugi::xml_node create_root(pugi::xml_document& document)
{
    return document.append_child("rom");
}

} // namespace

DefinitionHeaderInput normalize_header_input(DefinitionHeaderInput input)
{
    for (const auto member : {&DefinitionHeaderInput::xml_id, &DefinitionHeaderInput::internal_id,
                              &DefinitionHeaderInput::ecu_id, &DefinitionHeaderInput::include})
    {
        input.*member = trim_header_text(input.*member);
    }
    for (const auto& field : kEditableMetadataFields)
    {
        input.metadata.*field.member = trim_header_text(input.metadata.*field.member);
    }
    input.metadata.file_size = trim_header_text(input.metadata.file_size);
    return input;
}

Result<std::vector<std::uint8_t>> create_ecuflash_xml(const DefinitionHeaderInput& input)
{
    pugi::xml_document document;
    if (auto updated = update_header(create_root(document), input); !updated.has_value())
    {
        return std::unexpected(updated.error());
    }
    return serialize_and_validate(document);
}

Result<std::vector<std::uint8_t>> rewrite_ecuflash_xml(std::span<const std::uint8_t> source,
                                                       const DefinitionHeaderInput& input)
{
    pugi::xml_document document;
    constexpr unsigned int kParseFlags =
        pugi::parse_default | pugi::parse_comments | pugi::parse_declaration | pugi::parse_pi | pugi::parse_doctype;
    if (const pugi::xml_parse_result parsed =
            document.load_buffer(source.data(), source.size(), kParseFlags, pugi::encoding_auto);
        !parsed)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("EcuFlash source XML is malformed: {}", parsed.description()));
    }

    pugi::xml_node root = document.document_element();
    if (!root || root.name() != "rom"sv)
    {
        return fail(ErrorKind::InvalidConfig, "EcuFlash source root must be <rom>");
    }
    if (const auto rom_id = root.child("romid"); rom_id && rom_id.next_sibling("romid"))
    {
        return fail(ErrorKind::InvalidConfig, "EcuFlash source element <rom>: duplicate top-level <romid> elements");
    }
    if (auto updated = update_header(root, input); !updated.has_value())
    {
        return std::unexpected(updated.error());
    }
    return serialize_and_validate(document);
}

} // namespace fastecu::definition
