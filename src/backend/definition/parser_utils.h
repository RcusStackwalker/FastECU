#pragma once

#include <charconv>
#include <format>
#include <string>
#include <string_view>

#include <pugixml.hpp>

namespace fastecu::definition
{

inline std::string trim_copy(std::string_view value)
{
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
    {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
    {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

inline std::string detail_prefix(std::string_view source, std::string_view definition_id = {})
{
    std::string detail = "EcuFlash/RomRaider source '" + std::string(source) + "'";
    if (!definition_id.empty())
    {
        detail += ", definition '" + std::string(definition_id) + "'";
    }
    return detail + ": ";
}

inline std::unexpected<Error> invalid(
    std::string_view source,
    std::string context,
    std::string message,
    std::string_view definition_id = {})
{
    return fail(
        ErrorKind::InvalidConfig,
        detail_prefix(source, definition_id) + std::move(context) + ": " + std::move(message));
}

inline std::string child_text(pugi::xml_node parent, std::string_view child_name)
{
    const pugi::xml_node child = parent.child(child_name);
    return child ? trim_copy(child.child_value()) : std::string{};
}

inline Result<std::string> required_child_text(
    pugi::xml_node parent,
    std::string_view parent_name,
    std::string_view child_name,
    std::string_view source)
{
    const std::string value = child_text(parent, child_name);
    if (value.empty())
    {
        return invalid(
            source,
            std::format("element <{}> child <{}>", parent_name, child_name),
            "missing or empty required text");
    }
    return value;
}

inline Result<pugi::xml_node> parse_root(
    pugi::xml_document& document,
    std::span<const std::uint8_t> xml,
    std::string_view source,
    std::string_view root_name)
{
    const pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size());
    if (!parsed)
    {
        return invalid(source, "XML document", std::string("malformed XML: ") + parsed.description());
    }

    const pugi::xml_node root = document.document_element();
    if (!root || root.name() != root_name)
    {
        const std::string actual = root ? std::format("<{}>", root.name()) : "no root element";
        return invalid(source, std::format("root element <{}>", root_name), "wrong root; found " + actual);
    }
    return root;
}

inline Result<std::uint64_t> parse_hex_unsigned(
    std::string_view value,
    std::string_view source,
    std::string context,
    std::string_view definition_id)
{
    std::string trimmed = trim_copy(value);
    std::string_view digits = trimmed;
    if (digits.starts_with("0x") || digits.starts_with("0X"))
    {
        digits.remove_prefix(2);
    }

    std::uint64_t parsed = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), parsed, 16);
    if (error != std::errc{} || end != digits.data() + digits.size())
    {
        return invalid(
            source,
            std::move(context),
            std::format("invalid hexadecimal unsigned value '{}'", trimmed),
            definition_id);
    }
    return parsed;
}

inline std::string attribute_or_empty(pugi::xml_node node, std::string_view name)
{
    return trim_copy(node.attribute(name).value());
}

} // namespace fastecu::definition
