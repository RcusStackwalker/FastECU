#include "src/backend/definition/definition_xml_entities.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <set>
#include <utility>

#include <pugixml.hpp>

namespace fastecu::definition
{
namespace
{
constexpr std::size_t kExpansionLimit = 4096;

struct Entity
{
    std::string text;
    bool external = false;
};
using Entities = std::map<std::string, Entity, std::less<>>;

std::size_t characters(std::string_view text)
{
    return std::ranges::count_if(text, [](unsigned char byte) { return (byte & 0xc0) != 0x80; });
}

std::optional<std::uint32_t> character_reference(std::string_view name)
{
    name.remove_prefix(1); // '#'
    const int radix = name.starts_with('x') ? 16 : 10;
    if (radix == 16)
    {
        name.remove_prefix(1);
    }
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), value, radix);
    if (error != std::errc{} || end != name.data() + name.size())
    {
        return std::nullopt;
    }
    if (value == 9 || value == 10 || value == 13 || (value >= 0x20 && value <= 0xd7ff) ||
        (value >= 0xe000 && value <= 0xfffd) || (value >= 0x10000 && value <= 0x10ffff))
    {
        return value;
    }
    return std::nullopt;
}

void append_utf8(std::string& text, std::uint32_t value)
{
    if (value < 0x80)
    {
        text.push_back(static_cast<char>(value));
    }
    else if (value < 0x800)
    {
        text.push_back(static_cast<char>(0xc0 | (value >> 6)));
        text.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
    else if (value < 0x10000)
    {
        text.push_back(static_cast<char>(0xe0 | (value >> 12)));
        text.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
    else
    {
        text.push_back(static_cast<char>(0xf0 | (value >> 18)));
        text.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
}

// Numeric references in an entity declaration are decoded at declaration
// time. Predefined references stay encoded: &#60;b> is markup, &lt;b> is text.
std::optional<std::string> entity_value(std::string_view value)
{
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        if (value.substr(index).starts_with("&#"))
        {
            const auto end = value.find(';', index);
            if (end == std::string_view::npos)
            {
                return std::nullopt;
            }
            const auto code = character_reference(value.substr(index + 1, end - index - 1));
            if (!code.has_value())
            {
                return std::nullopt;
            }
            append_utf8(result, *code);
            index = end;
        }
        else
        {
            result.push_back(value[index]);
        }
    }
    return result;
}

void skip_space(std::string_view text, std::size_t& index)
{
    while (index < text.size() && std::string_view{" \t\r\n"}.find(text[index]) != std::string_view::npos)
    {
        ++index;
    }
}

// Find a markup end without mistaking quoted '>' or the internal subset for
// its end. Comments within a DTD can also contain brackets and quotes.
std::optional<std::size_t> markup_end(std::string_view text, std::size_t index)
{
    char quote = 0;
    int brackets = 0;
    for (; index < text.size(); ++index)
    {
        const char ch = text[index];
        if (quote != 0)
        {
            if (ch == quote)
            {
                quote = 0;
            }
        }
        else if (text.substr(index).starts_with("<!--"))
        {
            const auto end = text.find("-->", index + 4);
            if (end == std::string_view::npos)
            {
                return std::nullopt;
            }
            index = end + 2;
        }
        else if (ch == '\'' || ch == '"')
        {
            quote = ch;
        }
        else if (ch == '[')
        {
            ++brackets;
        }
        else if (ch == ']')
        {
            --brackets;
        }
        else if (ch == '>' && brackets == 0)
        {
            return index + 1;
        }
    }
    return std::nullopt;
}

bool declarations(std::string_view text, Entities& general, Entities& parameters, std::set<std::string>& active)
{
    for (std::size_t index = 0; index < text.size();)
    {
        skip_space(text, index);
        if (text.substr(index).starts_with("<!--"))
        {
            const auto end = text.find("-->", index + 4);
            if (end == std::string_view::npos)
            {
                return false;
            }
            index = end + 3;
        }
        else if (index < text.size() && text[index] == '%')
        {
            const auto end = text.find(';', index + 1);
            if (end == std::string_view::npos)
            {
                return false;
            }
            const std::string name{text.substr(index + 1, end - index - 1)};
            const auto entity = parameters.find(name);
            if (entity == parameters.end() || !active.insert(name).second || active.size() > kExpansionLimit)
            {
                return false;
            }
            if (characters(entity->second.text) > kExpansionLimit + characters(name) + 2 ||
                !declarations(entity->second.text, general, parameters, active))
            {
                return false;
            }
            active.erase(name);
            index = end + 1;
        }
        else if (text.substr(index).starts_with("<!ENTITY"))
        {
            index += 8;
            skip_space(text, index);
            const bool parameter = index < text.size() && text[index] == '%';
            if (parameter)
            {
                ++index;
                skip_space(text, index);
            }
            const auto start = index;
            while (index < text.size() && std::string_view{" \t\r\n"}.find(text[index]) == std::string_view::npos)
            {
                ++index;
            }
            const std::string name{text.substr(start, index - start)};
            skip_space(text, index);
            if (name.empty() || index >= text.size())
            {
                return false;
            }
            Entity entity;
            if (text[index] == '\'' || text[index] == '"')
            {
                const auto end = text.find(text[index], index + 1);
                if (end == std::string_view::npos)
                {
                    return false;
                }
                const auto value = entity_value(text.substr(index + 1, end - index - 1));
                if (!value.has_value())
                {
                    return false;
                }
                entity.text = *value;
                // Parameter references are not allowed inside declarations in
                // the internal subset. A literal percent sign is still text.
                for (std::size_t pos = entity.text.find('%'); pos != std::string::npos;
                     pos = entity.text.find('%', pos + 1))
                {
                    const auto semi = entity.text.find(';', pos);
                    if (semi != std::string::npos &&
                        entity.text.substr(pos, semi - pos).find_first_of(" \t\r\n") == std::string::npos)
                    {
                        return false;
                    }
                }
                index = end + 1;
            }
            else
            {
                entity.external = true;
            }
            const auto end = markup_end(text, index);
            if (!end.has_value())
            {
                return false;
            }
            (parameter ? parameters : general).try_emplace(name, std::move(entity));
            index = *end;
        }
        else if (text.substr(index).starts_with("<!"))
        {
            const auto end = markup_end(text, index);
            if (!end.has_value())
            {
                return false;
            }
            index = *end;
        }
        else if (index < text.size())
        {
            return false;
        }
    }
    return true;
}

std::optional<std::string> expand(std::string_view text, const Entities& entities, bool external_dtd,
                                  std::set<std::string>& active, bool attribute = false,
                                  std::optional<std::size_t> budget = std::nullopt)
{
    std::string result;
    bool tag = false;
    char quote = 0;
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        // Check during expansion, not only when returning, so repeated nested
        // references cannot allocate unbounded output before rejection.
        if (budget.has_value() && characters(result) > *budget)
        {
            return std::nullopt;
        }
        const auto tail = text.substr(index);
        std::size_t end = std::string_view::npos;
        std::size_t terminator = 0;
        if (tail.starts_with("<!--"))
        {
            end = text.find("-->", index + 4);
            terminator = 3;
        }
        else if (tail.starts_with("<![CDATA["))
        {
            end = text.find("]]>", index + 9);
            terminator = 3;
        }
        else if (tail.starts_with("<?"))
        {
            end = text.find("?>", index + 2);
            terminator = 2;
        }
        else if (tail.starts_with("<!DOCTYPE"))
        {
            const auto position = markup_end(text, index);
            if (!position.has_value())
            {
                return std::nullopt;
            }
            end = *position;
        }
        if (terminator != 0 || end != std::string_view::npos)
        {
            if (end == std::string_view::npos)
            {
                return std::nullopt;
            }
            end += terminator;
            result.append(text.substr(index, end - index));
            index = end - 1;
            continue;
        }
        if (text[index] == '&')
        {
            end = text.find(';', index + 1);
            if (end == std::string_view::npos)
            {
                return std::nullopt;
            }
            const auto name = text.substr(index + 1, end - index - 1);
            if (name == "amp" || name == "lt" || name == "gt" || name == "quot" || name == "apos" ||
                name.starts_with('#'))
            {
                if (name.starts_with('#') && !character_reference(name).has_value())
                {
                    return std::nullopt;
                }
                result.append(text.substr(index, end - index + 1));
            }
            else
            {
                const auto entity = entities.find(name);
                const bool in_attribute = attribute || quote != 0;
                if (entity == entities.end())
                {
                    if (!external_dtd || in_attribute)
                    {
                        return std::nullopt;
                    }
                }
                else if (entity->second.external)
                {
                    if (in_attribute)
                    {
                        return std::nullopt;
                    }
                }
                else
                {
                    const std::string owned_name{name};
                    if (!active.insert(owned_name).second || active.size() > kExpansionLimit)
                    {
                        return std::nullopt;
                    }
                    const auto value = expand(entity->second.text, entities, external_dtd, active, in_attribute,
                                              kExpansionLimit + characters(name) + 2);
                    active.erase(owned_name);
                    if (!value.has_value() || characters(*value) > kExpansionLimit + characters(name) + 2)
                    {
                        return std::nullopt;
                    }
                    for (const char ch : *value)
                    {
                        if (in_attribute && ch == '<')
                        {
                            return std::nullopt;
                        }
                        if (in_attribute && (ch == '\'' || ch == '"'))
                        {
                            result.append(ch == '\'' ? "&apos;" : "&quot;");
                        }
                        else
                        {
                            result.push_back(ch);
                        }
                    }
                }
            }
            index = end;
        }
        else
        {
            const char ch = text[index];
            if (static_cast<unsigned char>(ch) < 0x20 && ch != '\t' && ch != '\r' && ch != '\n')
            {
                return std::nullopt;
            }
            if (!attribute)
            {
                if (quote != 0)
                {
                    if (ch == quote)
                    {
                        quote = 0;
                    }
                }
                else if (tag && (ch == '\'' || ch == '"'))
                {
                    quote = ch;
                }
                else if (ch == '<')
                {
                    tag = true;
                }
                else if (ch == '>')
                {
                    tag = false;
                }
            }
            result.push_back(ch);
        }
    }
    if (budget.has_value() && characters(result) > *budget)
    {
        return std::nullopt;
    }
    return result;
}
} // namespace

std::optional<std::string> prepare_definition_xml(std::string_view xml)
{
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_doctype, pugi::encoding_utf8))
    {
        return std::nullopt;
    }
    Entities entities;
    Entities parameters;
    bool external_dtd = false;
    std::set<std::string> active;
    for (const auto node : document.children())
    {
        if (node.type() == pugi::node_doctype)
        {
            const std::string_view value{node.value()};
            const auto subset = value.find('[');
            const auto head = value.substr(0, subset);
            external_dtd =
                head.find("SYSTEM") != std::string_view::npos || head.find("PUBLIC") != std::string_view::npos;
            if (subset != std::string_view::npos)
            {
                const auto end = value.rfind(']');
                if (end == std::string_view::npos ||
                    !declarations(value.substr(subset + 1, end - subset - 1), entities, parameters, active))
                {
                    return std::nullopt;
                }
            }
        }
    }
    return expand(xml, entities, external_dtd, active);
}
} // namespace fastecu::definition
