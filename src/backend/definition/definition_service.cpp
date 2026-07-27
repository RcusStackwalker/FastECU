#include "src/backend/definition/definition_service.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/definition/definition_resolver.h"
#include "src/backend/definition/romraider_parser.h"

namespace fastecu::definition
{
namespace
{

std::string join_path(std::string_view directory, std::string_view name)
{
    if (directory.empty())
    {
        return std::string(name);
    }
    if (directory.back() == '/')
    {
        return std::string(directory) + std::string(name);
    }
    return std::string(directory) + "/" + std::string(name);
}

bool is_xml_handle(std::string_view handle)
{
    constexpr std::string_view suffix = ".xml";
    if (handle.size() < suffix.size())
    {
        return false;
    }
    const std::string_view candidate = handle.substr(handle.size() - suffix.size());
    return std::equal(
        candidate.begin(),
        candidate.end(),
        suffix.begin(),
        [](unsigned char left, unsigned char right)
        {
            return std::tolower(left) == std::tolower(right);
        });
}

std::optional<std::uint8_t> hex_nibble(char character)
{
    if (character >= '0' && character <= '9')
    {
        return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f')
    {
        return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    if (character >= 'A' && character <= 'F')
    {
        return static_cast<std::uint8_t>(character - 'A' + 10);
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> identifier_bytes(
    std::string_view identifier,
    IdEncoding encoding)
{
    if (identifier.empty())
    {
        return std::nullopt;
    }
    if (encoding == IdEncoding::Ascii)
    {
        return std::vector<std::uint8_t>(identifier.begin(), identifier.end());
    }
    if (identifier.size() % 2 != 0)
    {
        return std::nullopt;
    }

    std::vector<std::uint8_t> decoded;
    decoded.reserve(identifier.size() / 2);
    for (std::size_t index = 0; index < identifier.size(); index += 2)
    {
        auto high = hex_nibble(identifier[index]);
        auto low = hex_nibble(identifier[index + 1]);
        if (!high || !low)
        {
            return std::nullopt;
        }
        decoded.push_back(static_cast<std::uint8_t>((*high << 4U) | *low));
    }
    return decoded;
}

Result<void> discover_xml(
    IFileSystem& file_system,
    std::string_view directory,
    std::vector<std::string>& handles)
{
    auto entries = file_system.list_directory(directory);
    if (!entries)
    {
        return std::unexpected(entries.error());
    }

    for (const DirEntry& entry : *entries)
    {
        std::string handle = join_path(directory, entry.name);
        if (entry.is_directory)
        {
            auto nested = discover_xml(file_system, handle, handles);
            if (!nested)
            {
                return std::unexpected(nested.error());
            }
        }
        else if (is_xml_handle(handle))
        {
            handles.push_back(std::move(handle));
        }
    }
    return {};
}

template <typename Parser>
Result<DefinitionCatalog> build_catalog(
    IFileRepository& repository,
    std::span<const std::string> handles,
    Parser parser)
{
    std::vector<DefinitionIndexEntry> entries;
    for (const std::string& handle : handles)
    {
        auto contents = repository.read(handle);
        if (!contents)
        {
            return std::unexpected(contents.error());
        }
        auto parsed = parser(*contents, handle);
        if (!parsed)
        {
            return std::unexpected(parsed.error());
        }
        entries.insert(
            entries.end(),
            std::make_move_iterator(parsed->begin()),
            std::make_move_iterator(parsed->end()));
    }
    return DefinitionCatalog::create(std::move(entries));
}

} // namespace

DefinitionService::DefinitionService(
    IFileSystem& file_system,
    IFileRepository& repository,
    IAtomicFileWriter& writer)
    : file_system_(file_system), repository_(repository), writer_(writer)
{
}

Result<DefinitionCatalog> DefinitionService::build_romraider_catalog(
    std::span<const std::string> ordered_handles)
{
    return build_catalog(repository_, ordered_handles, parse_romraider_index);
}

Result<DefinitionCatalog> DefinitionService::build_ecuflash_catalog(std::string_view directory)
{
    std::vector<std::string> handles;
    auto discovered = discover_xml(file_system_, directory, handles);
    if (!discovered)
    {
        return std::unexpected(discovered.error());
    }
    std::sort(handles.begin(), handles.end());
    return build_catalog(repository_, handles, parse_ecuflash_index);
}

Result<DefinitionIndexEntry> DefinitionService::match_rom(
    const DefinitionCatalog& catalog,
    std::span<const std::uint8_t> rom) const
{
    for (const DefinitionIndexEntry& entry : catalog.entries())
    {
        auto candidate = identifier_bytes(entry.internal_id, entry.internal_id_encoding);
        if (!candidate || !entry.internal_id_address)
        {
            continue;
        }

        const std::uint64_t address = *entry.internal_id_address;
        if (address > rom.size())
        {
            continue;
        }
        const std::size_t offset = static_cast<std::size_t>(address);
        if (candidate->size() > rom.size() - offset)
        {
            continue;
        }
        if (std::equal(candidate->begin(), candidate->end(), rom.begin() + offset))
        {
            return entry;
        }
    }
    return fail(ErrorKind::InvalidConfig, "no matching ROM definition found");
}

Result<RomDefinition> DefinitionService::load(
    const DefinitionCatalog& catalog,
    DefinitionFormat format,
    std::string_view id)
{
    DefinitionLoader loader =
        [this, &catalog](
            DefinitionFormat requested_format,
            std::string_view requested_id) -> Result<UnresolvedDefinition>
    {
        auto found = catalog.find(requested_format, requested_id);
        if (!found)
        {
            return std::unexpected(found.error());
        }
        const DefinitionIndexEntry& entry = found->get();
        auto contents = repository_.read(entry.source);
        if (!contents)
        {
            return std::unexpected(contents.error());
        }

        if (requested_format == DefinitionFormat::RomRaider)
        {
            return parse_romraider_definition(*contents, entry.source, requested_id);
        }
        return parse_ecuflash_definition(*contents, entry.source);
    };

    auto root = loader(format, id);
    if (!root)
    {
        return std::unexpected(root.error());
    }
    return resolve_definition(std::move(*root), loader);
}

} // namespace fastecu::definition
