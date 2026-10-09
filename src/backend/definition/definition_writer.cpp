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

Status ValidateInput(const DefinitionHeaderInput& input)
{
    if (input.xml_id.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "definition XML ID is required");
    }
    if (input.internal_id.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "definition internal ID is required");
    }
    if (input.ecu_id.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "definition ECU ID is required");
    }
    return {};
}

void SetUniqueText(pugi::xml_node parent, const char *name, std::string_view value)
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

void SetOptionalHex(pugi::xml_node parent, const char *name, std::optional<std::uint64_t> value)
{
    if (value)
    {
        SetUniqueText(parent, name, HexText(*value));
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

Status UpdateHeader(pugi::xml_node root, const DefinitionHeaderInput& raw_input)
{
    const auto input = NormalizeHeaderInput(raw_input);
    if (auto valid = ValidateInput(input); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    pugi::xml_node rom_id = root.child("romid");
    if (!rom_id)
    {
        rom_id = root.prepend_child("romid");
    }
    SetUniqueText(rom_id, "xmlid", input.xml_id);
    SetOptionalHex(rom_id, "internalidaddress", input.internal_id_address);
    SetUniqueText(rom_id, "internalidstring", input.internal_id);
    SetUniqueText(rom_id, "ecuid", input.ecu_id);
    for (const auto& field : kEditableMetadataFields)
    {
        SetUniqueText(rom_id, field.xml_name, input.metadata.*field.member);
    }
    SetUniqueText(rom_id, "filesize", input.metadata.file_size);
    SetUniqueText(rom_id, "notes", input.metadata.notes);
    SetUniqueText(root, "include", input.include);
    SetUniqueText(root, "notes", input.notes);
    return {};
}

void NormalizeDeclaration(pugi::xml_document& document)
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

Result<std::vector<std::uint8_t>> SerializeAndValidate(pugi::xml_document& document)
{
    NormalizeDeclaration(document);
    std::ostringstream output;
    document.save(output, "  ", pugi::format_default, pugi::encoding_utf8);
    std::string xml = std::move(output).str();
    if (xml.empty() || xml.back() != '\n')
    {
        xml.push_back('\n');
    }

    std::vector<std::uint8_t> result(xml.begin(), xml.end());
    if (auto parsed = ParseEcuflashDefinition(result, "generated definition"); !parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    return result;
}

pugi::xml_node CreateRoot(pugi::xml_document& document)
{
    return document.append_child("rom");
}

} // namespace

DefinitionHeaderInput NormalizeHeaderInput(DefinitionHeaderInput input)
{
    for (const auto member : {&DefinitionHeaderInput::xml_id, &DefinitionHeaderInput::internal_id,
                              &DefinitionHeaderInput::ecu_id, &DefinitionHeaderInput::include})
    {
        input.*member = TrimHeaderText(input.*member);
    }
    for (const auto& field : kEditableMetadataFields)
    {
        input.metadata.*field.member = TrimHeaderText(input.metadata.*field.member);
    }
    input.metadata.file_size = TrimHeaderText(input.metadata.file_size);
    return input;
}

Result<std::vector<std::uint8_t>> CreateEcuflashXml(const DefinitionHeaderInput& input)
{
    pugi::xml_document document;
    if (auto updated = UpdateHeader(CreateRoot(document), input); !updated.has_value())
    {
        return std::unexpected(updated.error());
    }
    return SerializeAndValidate(document);
}

Result<std::vector<std::uint8_t>> RewriteEcuflashXml(std::span<const std::uint8_t> source,
                                                     const DefinitionHeaderInput& input)
{
    pugi::xml_document document;
    constexpr unsigned int kParseFlags =
        pugi::parse_default | pugi::parse_comments | pugi::parse_declaration | pugi::parse_pi | pugi::parse_doctype;
    if (const pugi::xml_parse_result parsed =
            document.load_buffer(source.data(), source.size(), kParseFlags, pugi::encoding_auto);
        !parsed)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("EcuFlash source XML is malformed: {}", parsed.description()));
    }

    pugi::xml_node root = document.document_element();
    if (!root || root.name() != "rom"sv)
    {
        return Fail(ErrorKind::kInvalidConfig, "EcuFlash source root must be <rom>");
    }
    if (const auto rom_id = root.child("romid"); rom_id && rom_id.next_sibling("romid"))
    {
        return Fail(ErrorKind::kInvalidConfig, "EcuFlash source element <rom>: duplicate top-level <romid> elements");
    }
    if (auto updated = UpdateHeader(root, input); !updated.has_value())
    {
        return std::unexpected(updated.error());
    }
    return SerializeAndValidate(document);
}

} // namespace fastecu::definition
