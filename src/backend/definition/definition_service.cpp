#include "src/backend/definition/definition_service.h"

#include <algorithm>
#include <cctype>
#include <format>
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

Result<std::vector<std::uint8_t>> identifier_bytes(
    std::string_view identifier,
    IdEncoding encoding)
{
    if (encoding == IdEncoding::Ascii)
    {
        return std::vector<std::uint8_t>(identifier.begin(), identifier.end());
    }
    if (identifier.size() % 2 != 0)
    {
        return fail(
            ErrorKind::InvalidConfig,
            "hexadecimal identifier has odd length");
    }

    std::vector<std::uint8_t> decoded;
    decoded.reserve(identifier.size() / 2);
    for (std::size_t index = 0; index < identifier.size(); index += 2)
    {
        auto high = hex_nibble(identifier[index]);
        auto low = hex_nibble(identifier[index + 1]);
        if (!high || !low)
        {
            return fail(
                ErrorKind::InvalidConfig,
                "identifier contains a non-hexadecimal digit");
        }
        decoded.push_back(static_cast<std::uint8_t>((*high << 4U) | *low));
    }
    return decoded;
}

Result<std::vector<std::vector<std::uint8_t>>> identifier_candidates(
    std::string_view identifier,
    IdEncoding encoding)
{
    if (encoding != IdEncoding::AsciiOrHex)
    {
        return identifier_bytes(identifier, encoding).transform([](auto&& candidate)
                                                                { return std::vector<std::vector<std::uint8_t>>{candidate}; });
    }

    std::vector<std::vector<std::uint8_t>> candidates{
        std::vector<std::uint8_t>(identifier.begin(), identifier.end()),
    };
    if (auto hexadecimal = identifier_bytes(identifier, IdEncoding::Hex); hexadecimal.has_value())
    {
        candidates.push_back(std::move(*hexadecimal));
    }
    return candidates;
}

std::unexpected<Error> invalid_match_metadata(
    const DefinitionIndexEntry& entry,
    std::string detail)
{
    return fail(
        ErrorKind::InvalidConfig,
        std::format("invalid ROM match metadata for definition '{}' from '{}': {}", entry.definition_id, entry.source, std::move(detail)));
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
            if (entry.is_symlink)
            {
                continue;
            }
            if (auto nested = discover_xml(file_system, handle, handles); !nested.has_value())
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
        if (!contents.has_value())
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

Result<DefinitionCatalog> DefinitionService::build_ecuflash_catalog(
    std::string_view directory,
    std::span<const std::string> explicit_handles)
{
    std::vector<std::string> handles;
    if (!directory.empty())
    {
        if (auto discovered = discover_xml(file_system_, directory, handles); !discovered.has_value())
        {
            return std::unexpected(discovered.error());
        }
    }
    for (const std::string& handle : explicit_handles)
    {
        if (is_xml_handle(handle))
        {
            handles.push_back(handle);
        }
    }
    std::ranges::sort(handles);
    const auto [first, last] = std::ranges::unique(handles);
    handles.erase(first, last);
    return build_catalog(repository_, handles, parse_ecuflash_index);
}

Result<DefinitionIndexEntry> DefinitionService::match_rom(
    const DefinitionCatalog& catalog,
    std::span<const std::uint8_t> rom) const
{
    for (const DefinitionIndexEntry& entry : catalog.entries())
    {
        if (entry.internal_id.empty())
        {
            continue;
        }
        auto candidates = identifier_candidates(
            entry.internal_id,
            entry.internal_id_encoding);
        if (!candidates.has_value())
        {
            return invalid_match_metadata(entry, candidates.error().detail);
        }
        if (!entry.internal_id_address.has_value())
        {
            return invalid_match_metadata(entry, "internal ID address is absent");
        }

        const std::uint64_t address = *entry.internal_id_address;
        if (address > rom.size())
        {
            return invalid_match_metadata(
                entry,
                "internal ID address " + std::to_string(address) +
                    " exceeds ROM size " + std::to_string(rom.size()));
        }
        const std::size_t offset = static_cast<std::size_t>(address);
        for (const std::vector<std::uint8_t>& candidate : *candidates)
        {
            if (candidate.size() <= rom.size() - offset &&
                std::equal(
                    candidate.begin(),
                    candidate.end(),
                    rom.begin() + offset))
            {
                return entry;
            }
        }
    }
    return fail(ErrorKind::InvalidConfig, "no matching ROM definition found");
}

Result<RomDefinition> DefinitionService::load(
    const DefinitionCatalog& catalog,
    DefinitionFormat format,
    std::string_view id)
{
    std::optional<Error> repository_error;
    DefinitionLoader loader =
        [this, &catalog, &repository_error](
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
            repository_error = contents.error();
            return std::unexpected(contents.error());
        }

        if (requested_format == DefinitionFormat::RomRaider)
        {
            return parse_romraider_definition(*contents, entry.source, requested_id);
        }
        auto parsed = parse_ecuflash_definition(*contents, entry.source);
        if (!parsed)
        {
            return std::unexpected(parsed.error());
        }
        if (parsed->identity.xml_id != requested_id)
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format("EcuFlash catalog ID '{}' from '{}' loaded definition '{}'", requested_id, entry.source, parsed->identity.xml_id));
        }
        return parsed;
    };

    auto root = loader(format, id);
    if (!root)
    {
        return std::unexpected(root.error());
    }
    auto resolved = resolve_definition(std::move(*root), loader);
    if (!resolved && repository_error)
    {
        return std::unexpected(*repository_error);
    }
    return resolved;
}

Status DefinitionService::create_definition(
    std::string_view destination,
    const DefinitionHeaderInput& input)
{
    auto contents = create_ecuflash_xml(input);
    if (!contents)
    {
        return std::unexpected(contents.error());
    }
    return writer_.replace(destination, *contents);
}

Status DefinitionService::import_definition(
    std::string_view source,
    std::string_view destination,
    const DefinitionHeaderInput& input)
{
    auto source_contents = repository_.read(source);
    if (!source_contents)
    {
        return std::unexpected(source_contents.error());
    }
    auto rewritten = rewrite_ecuflash_xml(*source_contents, input);
    if (!rewritten)
    {
        return std::unexpected(rewritten.error());
    }
    return writer_.replace(destination, *rewritten);
}

} // namespace fastecu::definition
