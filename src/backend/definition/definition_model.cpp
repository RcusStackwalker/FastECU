#include "src/backend/definition/definition_model.h"

#include "src/backend/ports/error.h"

#include <algorithm>
#include <cctype>
#include <expected>
#include <format>
#include <ranges>
#include <array>

namespace fastecu::definition
{
namespace
{

bool has_whitespace(std::string_view value)
{
    return std::ranges::any_of(value, [](unsigned char character) { return std::isspace(character) != 0; });
}

bool has_same_lookup_key(const DefinitionIndexEntry& left, const DefinitionIndexEntry& right)
{
    return left.format == right.format && left.definition_id == right.definition_id;
}

bool has_same_identity(const DefinitionIndexEntry& left, const DefinitionIndexEntry& right)
{
    return left.internal_id == right.internal_id && left.ecu_id == right.ecu_id;
}

bool has_same_content(const DefinitionIndexEntry& left, const DefinitionIndexEntry& right)
{
    return has_same_identity(left, right) && left.internal_id_address == right.internal_id_address &&
           left.internal_id_encoding == right.internal_id_encoding && left.parents == right.parents;
}

Result<void> validate(const DefinitionIndexEntry& entry)
{
    if (entry.definition_id.empty())
    {
        return fail(ErrorKind::InvalidConfig, "definition ID must not be empty");
    }
    if (entry.source.empty())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("definition source must not be empty for ID '{}'", entry.definition_id));
    }
    for (const std::string& parent : entry.parents)
    {
        if (parent.empty() || has_whitespace(parent))
        {
            return fail(ErrorKind::InvalidConfig, std::format("invalid parent reference in '{}'", entry.source));
        }
    }
    return {};
}

} // namespace

std::optional<StorageType> storage_type_from_text(std::string_view text)
{
    static constexpr std::array<std::pair<std::string_view, StorageType>, 10> kStorageTypes{{
        {"uint8", StorageType::Uint8},
        {"int8", StorageType::Int8},
        {"uint16", StorageType::Uint16},
        {"int16", StorageType::Int16},
        {"uint24", StorageType::Uint24},
        {"int24", StorageType::Int24},
        {"uint32", StorageType::Uint32},
        {"int32", StorageType::Int32},
        {"float", StorageType::Float},
        {"bloblist", StorageType::Bloblist},
    }};
    for (const auto& [name, value] : kStorageTypes)
    {
        if (text == name)
        {
            return value;
        }
    }
    return std::nullopt;
}

std::string storage_type_text(std::optional<StorageType> value)
{
    if (!value.has_value())
    {
        return {};
    }
    switch (*value)
    {
    case StorageType::Uint8:
        return "uint8";
    case StorageType::Int8:
        return "int8";
    case StorageType::Uint16:
        return "uint16";
    case StorageType::Int16:
        return "int16";
    case StorageType::Uint24:
        return "uint24";
    case StorageType::Int24:
        return "int24";
    case StorageType::Uint32:
        return "uint32";
    case StorageType::Int32:
        return "int32";
    case StorageType::Float:
        return "float";
    case StorageType::Bloblist:
        return "bloblist";
    }
    return {};
}

std::uint32_t storage_byte_size(std::optional<StorageType> storage_type)
{
    if (!storage_type.has_value())
    {
        return 1;
    }
    switch (*storage_type)
    {
    case StorageType::Uint16:
    case StorageType::Int16:
        return 2;
    case StorageType::Uint24:
    case StorageType::Int24:
        return 3;
    case StorageType::Uint32:
    case StorageType::Int32:
    case StorageType::Float:
        return 4;
    case StorageType::Uint8:
    case StorageType::Int8:
    case StorageType::Bloblist:
        return 1;
    }
    return 1;
}

const Scaling *find_scaling(const RomDefinition& definition, std::string_view name)
{
    const auto it = std::ranges::find(definition.scalings, name, &Scaling::name);
    return it != definition.scalings.end() ? &*it : nullptr;
}

bool is_unsigned_storage(std::optional<StorageType> storage_type)
{
    if (!storage_type.has_value())
    {
        return false;
    }
    switch (*storage_type)
    {
    case StorageType::Uint8:
    case StorageType::Uint16:
    case StorageType::Uint24:
    case StorageType::Uint32:
        return true;
    case StorageType::Int8:
    case StorageType::Int16:
    case StorageType::Int24:
    case StorageType::Int32:
    case StorageType::Float:
    case StorageType::Bloblist:
        return false;
    }
    return false;
}

DefinitionCatalog::DefinitionCatalog(std::vector<DefinitionIndexEntry> entries) : entries_(std::move(entries))
{
}

Result<DefinitionCatalog> DefinitionCatalog::create(std::vector<DefinitionIndexEntry> entries)
{
    std::vector<DefinitionIndexEntry> canonical_entries;
    canonical_entries.reserve(entries.size());

    for (DefinitionIndexEntry& entry : entries)
    {
        if (auto result = validate(entry); !result.has_value())
        {
            return std::unexpected(result.error());
        }

        auto existing = std::ranges::find_if(canonical_entries, [&entry](const DefinitionIndexEntry& candidate)
                                             { return has_same_lookup_key(candidate, entry); });
        if (existing == std::ranges::end(canonical_entries))
        {
            canonical_entries.push_back(std::move(entry));
            continue;
        }

        if (!has_same_content(*existing, entry))
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("conflicting duplicate definition ID '{}' from '{}' and '{}'", entry.definition_id,
                                    existing->source, entry.source));
        }
    }

    return DefinitionCatalog(std::move(canonical_entries));
}

Result<std::reference_wrapper<const DefinitionIndexEntry>> DefinitionCatalog::find(DefinitionFormat format,
                                                                                   std::string_view id) const
{
    auto entry = std::ranges::find_if(entries_, [format, id](const DefinitionIndexEntry& candidate)
                                      { return candidate.format == format && candidate.definition_id == id; });
    if (entry == std::ranges::end(entries_))
    {
        return fail(ErrorKind::InvalidConfig, std::format("definition ID not found: '{}'", id));
    }
    return std::cref(*entry);
}

std::span<const DefinitionIndexEntry> DefinitionCatalog::entries() const
{
    return entries_;
}

} // namespace fastecu::definition
