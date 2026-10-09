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

bool HasWhitespace(std::string_view value)
{
    return std::ranges::any_of(value, [](unsigned char character) { return std::isspace(character) != 0; });
}

bool HasSameLookupKey(const DefinitionIndexEntry& left, const DefinitionIndexEntry& right)
{
    return left.format == right.format && left.definition_id == right.definition_id;
}

bool HasSameIdentity(const DefinitionIndexEntry& left, const DefinitionIndexEntry& right)
{
    return left.internal_id == right.internal_id && left.ecu_id == right.ecu_id;
}

bool HasSameContent(const DefinitionIndexEntry& left, const DefinitionIndexEntry& right)
{
    return HasSameIdentity(left, right) && left.internal_id_address == right.internal_id_address &&
           left.internal_id_encoding == right.internal_id_encoding && left.parents == right.parents;
}

Result<void> Validate(const DefinitionIndexEntry& entry)
{
    if (entry.definition_id.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "definition ID must not be empty");
    }
    if (entry.source.empty())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("definition source must not be empty for ID '{}'", entry.definition_id));
    }
    for (const std::string& parent : entry.parents)
    {
        if (parent.empty() || HasWhitespace(parent))
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("invalid parent reference in '{}'", entry.source));
        }
    }
    return {};
}

} // namespace

std::optional<StorageType> StorageTypeFromText(std::string_view text)
{
    static constexpr std::array<std::pair<std::string_view, StorageType>, 10> kStorageTypes{{
        {"uint8", StorageType::kUint8},
        {"int8", StorageType::kInt8},
        {"uint16", StorageType::kUint16},
        {"int16", StorageType::kInt16},
        {"uint24", StorageType::kUint24},
        {"int24", StorageType::kInt24},
        {"uint32", StorageType::kUint32},
        {"int32", StorageType::kInt32},
        {"float", StorageType::kFloat},
        {"bloblist", StorageType::kBloblist},
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

std::string StorageTypeText(std::optional<StorageType> value)
{
    if (!value.has_value())
    {
        return {};
    }
    switch (*value)
    {
    case StorageType::kUint8:
        return "uint8";
    case StorageType::kInt8:
        return "int8";
    case StorageType::kUint16:
        return "uint16";
    case StorageType::kInt16:
        return "int16";
    case StorageType::kUint24:
        return "uint24";
    case StorageType::kInt24:
        return "int24";
    case StorageType::kUint32:
        return "uint32";
    case StorageType::kInt32:
        return "int32";
    case StorageType::kFloat:
        return "float";
    case StorageType::kBloblist:
        return "bloblist";
    }
    return {};
}

std::uint32_t StorageByteSize(std::optional<StorageType> storage_type)
{
    if (!storage_type.has_value())
    {
        return 1;
    }
    switch (*storage_type)
    {
    case StorageType::kUint16:
    case StorageType::kInt16:
        return 2;
    case StorageType::kUint24:
    case StorageType::kInt24:
        return 3;
    case StorageType::kUint32:
    case StorageType::kInt32:
    case StorageType::kFloat:
        return 4;
    case StorageType::kUint8:
    case StorageType::kInt8:
    case StorageType::kBloblist:
        return 1;
    }
    return 1;
}

const Scaling *FindScaling(const RomDefinition& definition, std::string_view name)
{
    const auto it = std::ranges::find(definition.scalings, name, &Scaling::name);
    return it != definition.scalings.end() ? &*it : nullptr;
}

bool IsUnsignedStorage(std::optional<StorageType> storage_type)
{
    if (!storage_type.has_value())
    {
        return false;
    }
    switch (*storage_type)
    {
    case StorageType::kUint8:
    case StorageType::kUint16:
    case StorageType::kUint24:
    case StorageType::kUint32:
        return true;
    case StorageType::kInt8:
    case StorageType::kInt16:
    case StorageType::kInt24:
    case StorageType::kInt32:
    case StorageType::kFloat:
    case StorageType::kBloblist:
        return false;
    }
    return false;
}

DefinitionCatalog::DefinitionCatalog(std::vector<DefinitionIndexEntry> entries) : entries_(std::move(entries))
{
}

Result<DefinitionCatalog> DefinitionCatalog::Create(std::vector<DefinitionIndexEntry> entries)
{
    std::vector<DefinitionIndexEntry> canonical_entries;
    canonical_entries.reserve(entries.size());

    for (DefinitionIndexEntry& entry : entries)
    {
        if (auto result = Validate(entry); !result.has_value())
        {
            return std::unexpected(result.error());
        }

        auto existing = std::ranges::find_if(canonical_entries, [&entry](const DefinitionIndexEntry& candidate)
                                             { return HasSameLookupKey(candidate, entry); });
        if (existing == std::ranges::end(canonical_entries))
        {
            canonical_entries.push_back(std::move(entry));
            continue;
        }

        if (!HasSameContent(*existing, entry))
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("conflicting duplicate definition ID '{}' from '{}' and '{}'", entry.definition_id,
                                    existing->source, entry.source));
        }
    }

    return DefinitionCatalog(std::move(canonical_entries));
}

Result<std::reference_wrapper<const DefinitionIndexEntry>> DefinitionCatalog::Find(DefinitionFormat format,
                                                                                   std::string_view id) const
{
    auto entry = std::ranges::find_if(entries_, [format, id](const DefinitionIndexEntry& candidate)
                                      { return candidate.format == format && candidate.definition_id == id; });
    if (entry == std::ranges::end(entries_))
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("definition ID not found: '{}'", id));
    }
    return std::cref(*entry);
}

std::span<const DefinitionIndexEntry> DefinitionCatalog::Entries() const
{
    return entries_;
}

} // namespace fastecu::definition
