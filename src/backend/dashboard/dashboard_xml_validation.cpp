#include "src/backend/dashboard/dashboard_xml_validation.h"

#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <pugixml.hpp>

namespace fastecu::dashboard
{
namespace
{
std::unexpected<Error> invalid(std::string path, std::string explanation)
{
    return fail(ErrorKind::InvalidConfig, std::move(path) + ": " + std::move(explanation));
}

bool is_xml_character(std::uint32_t code_point)
{
    return code_point == 0x9 || code_point == 0xa || code_point == 0xd ||
           (code_point >= 0x20 && code_point <= 0xd7ff) || (code_point >= 0xe000 && code_point <= 0xfffd) ||
           (code_point >= 0x10000 && code_point <= 0x10ffff);
}

Status reject_illegal_numeric_character_references_in_text(std::string_view text, const std::string& path)
{
    std::size_t offset = 0;
    while ((offset = text.find("&#", offset)) != std::string_view::npos)
    {
        const std::size_t end = text.find(';', offset + 2);
        if (end == std::string_view::npos)
        {
            return invalid(path, "invalid numeric XML character reference");
        }
        std::string_view digits = text.substr(offset + 2, end - offset - 2);
        int base = 10;
        if (digits.starts_with('x'))
        {
            digits.remove_prefix(1);
            base = 16;
        }
        std::uint32_t code_point = 0;
        const auto [parsed_end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), code_point, base);
        if (digits.empty() || error != std::errc{} || parsed_end != digits.data() + digits.size() ||
            !is_xml_character(code_point))
        {
            return invalid(path, "invalid numeric XML character reference");
        }
        offset = end + 1;
    }
    return {};
}

Status reject_illegal_numeric_character_references(pugi::xml_node node, const std::string& path)
{
    if (node.type() == pugi::node_pcdata)
    {
        return reject_illegal_numeric_character_references_in_text(node.value(), path);
    }
    if (node.type() != pugi::node_document && node.type() != pugi::node_element)
    {
        return {};
    }
    for (const pugi::xml_attribute attribute : node.attributes())
    {
        if (Status status = reject_illegal_numeric_character_references_in_text(attribute.value(), path); !status)
        {
            return status;
        }
    }
    for (const pugi::xml_node child : node.children())
    {
        if (Status status = reject_illegal_numeric_character_references(child, path); !status)
        {
            return status;
        }
    }
    return {};
}
} // namespace

Status validate_xml_text(std::string_view value, std::string path)
{
    constexpr std::string_view explanation = "must contain only valid UTF-8 XML 1.0 characters";
    std::size_t offset = 0;
    while (offset < value.size())
    {
        const auto lead = static_cast<std::uint8_t>(value[offset]);
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        std::size_t continuation_count = 0;
        if (lead <= 0x7f)
        {
            code_point = lead;
        }
        else if ((lead & 0xe0) == 0xc0)
        {
            code_point = lead & 0x1f;
            minimum = 0x80;
            continuation_count = 1;
        }
        else if ((lead & 0xf0) == 0xe0)
        {
            code_point = lead & 0x0f;
            minimum = 0x800;
            continuation_count = 2;
        }
        else if ((lead & 0xf8) == 0xf0)
        {
            code_point = lead & 0x07;
            minimum = 0x10000;
            continuation_count = 3;
        }
        else
        {
            return invalid(std::move(path), std::string(explanation));
        }

        if (continuation_count > value.size() - offset - 1)
        {
            return invalid(std::move(path), std::string(explanation));
        }
        for (std::size_t index = 1; index <= continuation_count; ++index)
        {
            const auto continuation = static_cast<std::uint8_t>(value[offset + index]);
            if ((continuation & 0xc0) != 0x80)
            {
                return invalid(std::move(path), std::string(explanation));
            }
            code_point = (code_point << 6) | (continuation & 0x3f);
        }
        if ((continuation_count != 0 && code_point < minimum) || !is_xml_character(code_point))
        {
            return invalid(std::move(path), std::string(explanation));
        }
        offset += continuation_count + 1;
    }
    return {};
}

Status validate_xml_input(bytes::ByteView xml, std::string path)
{
    const std::string_view source =
        xml.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char *>(xml.data()), xml.size());
    if (Status status = validate_xml_text(source, path); !status)
    {
        return status;
    }

    pugi::xml_document lexical_tree;
    constexpr unsigned int lexical_flags = pugi::parse_default & ~pugi::parse_escapes;
    const auto parsed = lexical_tree.load_buffer(xml.data(), xml.size(), lexical_flags, pugi::encoding_utf8);
    if (!parsed)
    {
        return invalid(std::move(path), std::format("{} at offset {}", parsed.description(), parsed.offset));
    }
    return reject_illegal_numeric_character_references(lexical_tree, path);
}

Status validate_dashboard_document_xml_strings(const DashboardDocument& document)
{
    if (Status status = validate_xml_text(document.metadata.name, "metadata.name"); !status)
    {
        return status;
    }
    if (document.metadata.description)
    {
        if (Status status = validate_xml_text(*document.metadata.description, "metadata.description"); !status)
        {
            return status;
        }
    }
    if (document.connection.preferred_adapter)
    {
        if (Status status =
                validate_xml_text(document.connection.preferred_adapter->vendor, "connection.preferred-adapter.vendor");
            !status)
        {
            return status;
        }
        if (Status status = validate_xml_text(document.connection.preferred_adapter->display_name,
                                              "connection.preferred-adapter.display-name");
            !status)
        {
            return status;
        }
    }

    for (std::size_t channel_index = 0; channel_index < document.channels.size(); ++channel_index)
    {
        const DashboardChannel& channel = document.channels[channel_index];
        const std::string indexed_path = "channels[" + std::to_string(channel_index) + "]";
        if (Status status = validate_xml_text(channel.id, indexed_path + ".id"); !status)
        {
            return status;
        }
        const std::string channel_path = "channels[" + channel.id + "]";
        if (Status status = validate_xml_text(channel.name, channel_path + ".name"); !status)
        {
            return status;
        }
        if (Status status = validate_xml_text(channel.description, channel_path + ".description"); !status)
        {
            return status;
        }

        for (std::size_t conversion_index = 0; conversion_index < channel.conversions.size(); ++conversion_index)
        {
            const DashboardConversion& conversion = channel.conversions[conversion_index];
            const std::string indexed_conversion_path =
                channel_path + ".conversions[" + std::to_string(conversion_index) + "]";
            if (Status status = validate_xml_text(conversion.id, indexed_conversion_path + ".id"); !status)
            {
                return status;
            }
            const std::string conversion_path = channel_path + ".conversions[" + conversion.id + "]";
            if (Status status = validate_xml_text(conversion.expression, conversion_path + ".expression"); !status)
            {
                return status;
            }
            if (Status status = validate_xml_text(conversion.unit, conversion_path + ".unit"); !status)
            {
                return status;
            }
        }
    }

    for (std::size_t card_index = 0; card_index < document.cards.size(); ++card_index)
    {
        const DashboardCard& card = document.cards[card_index];
        const std::string indexed_path = "cards[" + std::to_string(card_index) + "]";
        if (Status status = validate_xml_text(card.id, indexed_path + ".id"); !status)
        {
            return status;
        }
        const std::string card_path = "cards[" + card.id + "]";
        if (Status status = validate_xml_text(card.channel_id, card_path + ".channel-id"); !status)
        {
            return status;
        }
        if (Status status = validate_xml_text(card.conversion_id, card_path + ".conversion-id"); !status)
        {
            return status;
        }
        if (card.title)
        {
            if (Status status = validate_xml_text(*card.title, card_path + ".title"); !status)
            {
                return status;
            }
        }
    }
    return {};
}
} // namespace fastecu::dashboard
