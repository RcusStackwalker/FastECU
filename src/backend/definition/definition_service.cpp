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

std::string JoinPath(std::string_view directory, std::string_view name)
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

bool IsXmlHandle(std::string_view handle)
{
    constexpr std::string_view kSuffix = ".xml";
    if (handle.size() < kSuffix.size())
    {
        return false;
    }
    const std::string_view candidate = handle.substr(handle.size() - kSuffix.size());
    return std::equal(candidate.begin(), candidate.end(), kSuffix.begin(), [](unsigned char left, unsigned char right)
                      { return std::tolower(left) == std::tolower(right); });
}

std::optional<unsigned> HexNibble(char character)
{
    if (character >= '0' && character <= '9')
    {
        return static_cast<unsigned>(character - '0');
    }
    if (character >= 'a' && character <= 'f')
    {
        return static_cast<unsigned>(character - 'a' + 10);
    }
    if (character >= 'A' && character <= 'F')
    {
        return static_cast<unsigned>(character - 'A' + 10);
    }
    return std::nullopt;
}

Result<std::vector<std::uint8_t>> IdentifierBytes(std::string_view identifier, IdEncoding encoding)
{
    if (encoding == IdEncoding::kAscii)
    {
        return std::vector<std::uint8_t>(identifier.begin(), identifier.end());
    }
    if (identifier.size() % 2 != 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "hexadecimal identifier has odd length");
    }

    std::vector<std::uint8_t> decoded;
    decoded.reserve(identifier.size() / 2);
    for (std::size_t index = 0; index < identifier.size(); index += 2)
    {
        auto high = HexNibble(identifier[index]);
        auto low = HexNibble(identifier[index + 1]);
        if (!high.has_value() || !low.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "identifier contains a non-hexadecimal digit");
        }
        decoded.push_back(static_cast<std::uint8_t>((*high << 4U) | *low));
    }
    return decoded;
}

Result<std::vector<std::vector<std::uint8_t>>> IdentifierCandidates(std::string_view identifier, IdEncoding encoding)
{
    if (encoding != IdEncoding::kAsciiOrHex)
    {
        auto result = IdentifierBytes(identifier, encoding);
        if (!result.has_value())
        {
            return std::unexpected(std::move(result).error());
        }
        return std::vector<std::vector<std::uint8_t>>{*std::move(result)};
    }

    std::vector<std::vector<std::uint8_t>> candidates{
        std::vector<std::uint8_t>(identifier.begin(), identifier.end()),
    };
    if (auto hexadecimal = IdentifierBytes(identifier, IdEncoding::kHex); hexadecimal.has_value())
    {
        candidates.push_back(std::move(*hexadecimal));
    }
    return candidates;
}

Result<void> DiscoverXml(IFileSystem& file_system, std::string_view directory, std::vector<std::string>& handles)
{
    auto entries = file_system.ListDirectory(directory);
    if (!entries.has_value())
    {
        return std::unexpected(entries.error());
    }

    for (const DirEntry& entry : *entries)
    {
        std::string handle = JoinPath(directory, entry.name);
        if (entry.is_directory)
        {
            if (entry.is_symlink)
            {
                continue;
            }
            if (auto nested = DiscoverXml(file_system, handle, handles); !nested.has_value())
            {
                return std::unexpected(nested.error());
            }
        }
        else if (IsXmlHandle(handle))
        {
            handles.push_back(std::move(handle));
        }
    }
    return {};
}

// Individual bad handles (unreadable, malformed, or the wrong format's XML sharing a
// directory) are skipped rather than failing the whole catalog: a definitions directory
// mixing formats or containing one broken file is normal, and one broken entry must not
// hide every other ROM this ECU family could otherwise identify.
template <typename Parser>
Result<DefinitionCatalog> BuildCatalog(IFileRepository& repository, std::span<const std::string> handles, Parser parser,
                                       bool skip_unusable_handles)
{
    std::vector<DefinitionIndexEntry> entries;
    for (const std::string& handle : handles)
    {
        auto contents = repository.Read(handle);
        if (!contents.has_value())
        {
            if (skip_unusable_handles)
            {
                continue;
            }
            return std::unexpected(contents.error());
        }
        auto parsed = parser(*contents, handle);
        if (!parsed.has_value())
        {
            if (skip_unusable_handles)
            {
                continue;
            }
            return std::unexpected(parsed.error());
        }
        entries.insert(entries.end(), std::make_move_iterator(parsed->begin()), std::make_move_iterator(parsed->end()));
    }
    return DefinitionCatalog::Create(std::move(entries));
}

} // namespace

DefinitionService::DefinitionService(IFileSystem& file_system, IFileRepository& repository, IAtomicFileWriter& writer)
    : file_system_(file_system), repository_(repository), writer_(writer)
{
}

Result<DefinitionCatalog> DefinitionService::BuildRomraiderCatalog(std::span<const std::string> ordered_handles,
                                                                   bool skip_unusable_handles)
{
    return BuildCatalog(repository_, ordered_handles, ParseRomraiderIndex, skip_unusable_handles);
}

Result<DefinitionCatalog> DefinitionService::BuildEcuflashCatalog(std::string_view directory,
                                                                  std::span<const std::string> explicit_handles,
                                                                  bool skip_unusable_handles)
{
    std::vector<std::string> handles;
    if (!directory.empty())
    {
        if (auto discovered = DiscoverXml(file_system_, directory, handles); !discovered.has_value())
        {
            return std::unexpected(discovered.error());
        }
    }
    for (const std::string& handle : explicit_handles)
    {
        if (IsXmlHandle(handle))
        {
            handles.push_back(handle);
        }
    }
    std::ranges::sort(handles);
    const auto [first, last] = std::ranges::unique(handles);
    handles.erase(first, last);
    return BuildCatalog(repository_, handles, ParseEcuflashIndex, skip_unusable_handles);
}

Result<DefinitionIndexEntry> DefinitionService::MatchRom(const DefinitionCatalog& catalog,
                                                         std::span<const std::uint8_t> rom) const
{
    for (const DefinitionIndexEntry& entry : catalog.Entries())
    {
        if (entry.internal_id.empty())
        {
            continue;
        }
        // An entry with unusable match metadata (bad identifier encoding, no address, or an
        // address outside this particular ROM) just isn't a candidate for this ROM -- it is
        // not a reason to abandon the scan before reaching a later entry that does match.
        auto candidates = IdentifierCandidates(entry.internal_id, entry.internal_id_encoding);
        if (!candidates.has_value())
        {
            continue;
        }
        if (!entry.internal_id_address.has_value())
        {
            continue;
        }

        const std::uint64_t address = *entry.internal_id_address;
        if (address > rom.size())
        {
            continue;
        }
        const auto offset = static_cast<std::size_t>(address);
        for (const std::vector<std::uint8_t>& candidate : *candidates)
        {
            if (candidate.size() <= rom.size() - offset &&
                std::ranges::equal(candidate, rom.subspan(offset, candidate.size())))
            {
                return entry;
            }
        }
    }
    return Fail(ErrorKind::kInvalidConfig, "no matching ROM definition found");
}

Result<RomDefinition> DefinitionService::Load(const DefinitionCatalog& catalog, DefinitionFormat format,
                                              std::string_view id)
{
    std::optional<Error> repository_error;
    DefinitionLoader loader = [this, &catalog,
                               &repository_error](DefinitionFormat requested_format,
                                                  std::string_view requested_id) -> Result<UnresolvedDefinition>
    {
        auto found = catalog.Find(requested_format, requested_id);
        if (!found.has_value())
        {
            return std::unexpected(found.error());
        }
        const DefinitionIndexEntry& entry = found->get();
        auto contents = repository_.Read(entry.source);
        if (!contents.has_value())
        {
            repository_error = contents.error();
            return std::unexpected(contents.error());
        }

        if (requested_format == DefinitionFormat::kRomRaider)
        {
            return ParseRomraiderDefinition(*contents, entry.source, requested_id);
        }
        auto parsed = ParseEcuflashDefinition(*contents, entry.source);
        if (!parsed.has_value())
        {
            return std::unexpected(parsed.error());
        }
        if (parsed->identity.xml_id != requested_id)
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("EcuFlash catalog ID '{}' from '{}' loaded definition '{}'", requested_id,
                                    entry.source, parsed->identity.xml_id));
        }
        return parsed;
    };

    auto root = loader(format, id);
    if (!root.has_value())
    {
        return std::unexpected(root.error());
    }
    auto resolved = ResolveDefinition(std::move(*root), loader);
    if (!resolved.has_value() && repository_error)
    {
        return std::unexpected(*repository_error);
    }
    return resolved;
}

Status DefinitionService::CreateDefinition(std::string_view destination, const DefinitionHeaderInput& input,
                                           bool allow_overwrite)
{
    // "create" means a brand new definition file; import_definition is the rewrite-an-existing-
    // file path. Without this check a filename collision would silently replace -- and lose --
    // whatever definition was already at that destination. Callers whose own UI already
    // confirmed the overwrite (e.g. a native Save-As dialog) pass allow_overwrite=true.
    if (!allow_overwrite && file_system_.Exists(destination))
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("definition destination '{}' already exists", destination));
    }
    auto contents = CreateEcuflashXml(input);
    if (!contents.has_value())
    {
        return std::unexpected(contents.error());
    }
    return writer_.Replace(destination, *contents);
}

Status DefinitionService::ImportDefinition(std::string_view source, std::string_view destination,
                                           const DefinitionHeaderInput& input)
{
    auto source_contents = repository_.Read(source);
    if (!source_contents.has_value())
    {
        return std::unexpected(source_contents.error());
    }
    auto rewritten = RewriteEcuflashXml(*source_contents, input);
    if (!rewritten.has_value())
    {
        return std::unexpected(rewritten.error());
    }
    return writer_.Replace(destination, *rewritten);
}

} // namespace fastecu::definition
