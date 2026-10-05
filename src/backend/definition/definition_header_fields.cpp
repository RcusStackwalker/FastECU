#include "src/backend/definition/definition_header_fields.h"

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>

#include <pugixml.hpp>

#include "src/backend/definition/text_format.h"
#include "src/backend/definition/parser_utils.h"

namespace fastecu::definition
{
namespace
{
// UTF-8 encodings of Unicode White_Space, independent of locale.
constexpr auto kWhitespace = std::to_array<std::string_view>({" ",
                                                              "\t",
                                                              "\n",
                                                              "\v",
                                                              "\f",
                                                              "\r",
                                                              "\xc2\x85",
                                                              "\xc2\xa0",
                                                              "\xe1\x9a\x80",
                                                              "\xe2\x80\x80",
                                                              "\xe2\x80\x81",
                                                              "\xe2\x80\x82",
                                                              "\xe2\x80\x83",
                                                              "\xe2\x80\x84",
                                                              "\xe2\x80\x85",
                                                              "\xe2\x80\x86",
                                                              "\xe2\x80\x87",
                                                              "\xe2\x80\x88",
                                                              "\xe2\x80\x89",
                                                              "\xe2\x80\x8a",
                                                              "\xe2\x80\xa8",
                                                              "\xe2\x80\xa9",
                                                              "\xe2\x80\xaf",
                                                              "\xe2\x81\x9f",
                                                              "\xe3\x80\x80"});

std::string_view trim_header_text(std::string_view text)
{
    for (;;)
    {
        const auto space = std::ranges::find_if(kWhitespace, [text](auto value) { return text.starts_with(value); });
        if (space == kWhitespace.end())
        {
            break;
        }
        text.remove_prefix(space->size());
    }
    for (;;)
    {
        const auto space = std::ranges::find_if(kWhitespace, [text](auto value) { return text.ends_with(value); });
        if (space == kWhitespace.end())
        {
            break;
        }
        text.remove_suffix(space->size());
    }
    return text;
}

} // namespace

DefinitionHeaderFields collect_ecuflash_base_header_fields(std::span<const std::string> names, std::string_view xml)
{
    pugi::xml_document document;
    pugi::xml_node root;
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t *>(xml.data()), xml.size()};
    const auto parsed = parse_document_root(document, bytes, "authoring header", pugi::encoding_utf8);
    if (parsed &&
        std::ranges::count_if(document.children(), [](auto node) { return node.type() == pugi::node_element; }) == 1)
    {
        const auto document_root = *parsed;
        const std::string_view name{document_root.name()};
        if (name == "rom")
        {
            root = document_root;
        }
        else if (name == "roms")
        {
            root = document_root.child("rom");
        }
    }
    const auto rom_id = root.child("romid");
    DefinitionHeaderFields fields;
    fields.reserve(names.size());
    for (const auto& name : names)
    {
        const auto element =
            (name == "include" || name == "notes") ? root.child(name.c_str()) : rom_id.child(name.c_str());
        fields.emplace_back(name, read_element_text(element, XmlTextMode::DescendantText));
    }
    return fields;
}

Result<DefinitionHeaderInput> definition_header_input(std::span<const std::pair<std::string, std::string>> fields)
{
    const auto value = [fields](std::string_view name) -> std::string_view
    {
        for (const auto& [field_name, text] : fields | std::views::reverse)
        {
            if (field_name == name)
            {
                return text;
            }
        }
        return {};
    };
    std::optional<std::uint64_t> address;
    if (auto text = trim_header_text(value("internalidaddress")); !text.empty())
    {
        if (text.starts_with('+'))
        {
            text.remove_prefix(1);
        }
        // parse_hex_value also trims ASCII whitespace; a space after '+' is
        // not surrounding whitespace and must remain an error.
        address = trim_header_text(text) == text ? parse_hex_value(text) : std::nullopt;
        if (!address.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "definition internal ID address is not a valid integer");
        }
    }
    return DefinitionHeaderInput{.xml_id = std::string{trim_header_text(value("xmlid"))},
                                 .internal_id = std::string{value("internalidstring")},
                                 .ecu_id = std::string{value("ecuid")},
                                 .internal_id_address = address,
                                 .metadata = RomMetadata{.make = std::string{value("make")},
                                                         .market = std::string{value("market")},
                                                         .model = std::string{value("model")},
                                                         .submodel = std::string{value("submodel")},
                                                         .transmission = std::string{value("transmission")},
                                                         .year = std::string{value("year")},
                                                         .flash_method = std::string{value("flashmethod")},
                                                         .memory_model = std::string{value("memmodel")},
                                                         .checksum_module = std::string{value("checksummodule")}},
                                 .include = std::string{value("include")},
                                 .notes = std::string{value("notes")}};
}
} // namespace fastecu::definition
