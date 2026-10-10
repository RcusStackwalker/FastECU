#include "src/backend/definition/definition_resolver.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/backend/definition/text_format.h"

namespace fastecu::definition
{
namespace
{

std::string FormatName(DefinitionFormat format)
{
    return format == DefinitionFormat::kRomRaider ? "RomRaider" : "EcuFlash";
}

// The map's id when it is non-empty (the only kind usable as a stable key), else null.
const std::string *StableMapId(const UnresolvedCalibrationMap& map)
{
    return map.id.has_value() && !map.id->empty() ? &*map.id : nullptr;
}

bool HasStableMapId(const UnresolvedCalibrationMap& map)
{
    return StableMapId(map) != nullptr;
}

std::string MapKey(const UnresolvedCalibrationMap& map)
{
    const std::string *id = StableMapId(map);
    return id != nullptr ? *id : map.name;
}

bool MapsMatch(const UnresolvedCalibrationMap& left, const UnresolvedCalibrationMap& right)
{
    if (HasStableMapId(left) && HasStableMapId(right))
    {
        return left.id == right.id;
    }
    return left.name == right.name;
}

std::string MapKey(const CalibrationMap& map)
{
    return !map.id.empty() ? map.id : map.name;
}

bool MapsMatch(const CalibrationMap& left, const CalibrationMap& right)
{
    if (!left.id.empty() && !right.id.empty())
    {
        return left.id == right.id;
    }
    return left.name == right.name;
}

void OverlayString(std::string& value, std::string_view supplied)
{
    if (!supplied.empty())
    {
        value = supplied;
    }
}

template <typename Value> void OverlayOptional(Value& value, const Value& supplied)
{
    if (supplied)
    {
        value = supplied;
    }
}

void OverlayIdentity(RomIdentity& value, const RomIdentity& supplied)
{
    value.xml_id = supplied.xml_id;
    OverlayString(value.internal_id, supplied.internal_id);
    OverlayString(value.ecu_id, supplied.ecu_id);
    OverlayOptional(value.internal_id_address, supplied.internal_id_address);
}

void OverlayMetadata(RomMetadata& value, const RomMetadata& supplied)
{
    OverlayString(value.make, supplied.make);
    OverlayString(value.market, supplied.market);
    OverlayString(value.model, supplied.model);
    OverlayString(value.submodel, supplied.submodel);
    OverlayString(value.transmission, supplied.transmission);
    OverlayString(value.year, supplied.year);
    OverlayString(value.flash_method, supplied.flash_method);
    OverlayString(value.memory_model, supplied.memory_model);
    OverlayString(value.checksum_module, supplied.checksum_module);
    OverlayString(value.file_size, supplied.file_size);
    OverlayString(value.notes, supplied.notes);
}

void OverlayAxis(UnresolvedAxisDefinition& value, const UnresolvedAxisDefinition& supplied)
{
    if (supplied == UnresolvedAxisDefinition{})
    {
        return;
    }
    OverlayString(value.type, supplied.type);
    OverlayString(value.name, supplied.name);
    OverlayString(value.units, supplied.units);
    OverlayString(value.format, supplied.format);
    OverlayOptional(value.storage_type, supplied.storage_type);
    OverlayString(value.endian, supplied.endian);
    OverlayOptional(value.address, supplied.address);
    OverlayOptional(value.size, supplied.size);
    OverlayOptional(value.from_byte, supplied.from_byte);
    OverlayOptional(value.to_byte, supplied.to_byte);
    OverlayString(value.scaling_name, supplied.scaling_name);
    OverlayOptional(value.start_position, supplied.start_position);
    OverlayOptional(value.interval, supplied.interval);
    OverlayOptional(value.log_parameter, supplied.log_parameter);
    OverlayOptional(value.static_data, supplied.static_data);
}

void OverlayMap(UnresolvedCalibrationMap& value, const UnresolvedCalibrationMap& supplied)
{
    if (HasStableMapId(supplied))
    {
        value.id = supplied.id;
    }
    OverlayString(value.name, supplied.name);
    OverlayString(value.type, supplied.type);
    OverlayString(value.category, supplied.category);
    OverlayString(value.subcategory, supplied.subcategory);
    OverlayString(value.description, supplied.description);
    OverlayOptional(value.address, supplied.address);
    OverlayOptional(value.x_size, supplied.x_size);
    OverlayOptional(value.y_size, supplied.y_size);
    OverlayOptional(value.swap_xy, supplied.swap_xy);
    OverlayOptional(value.flip_x, supplied.flip_x);
    OverlayOptional(value.flip_y, supplied.flip_y);
    OverlayString(value.level, supplied.level);
    OverlayString(value.user_level, supplied.user_level);
    OverlayString(value.scaling_name, supplied.scaling_name);
    OverlayOptional(value.storage_type, supplied.storage_type);
    OverlayString(value.endian, supplied.endian);
    OverlayOptional(value.start_position, supplied.start_position);
    OverlayOptional(value.interval, supplied.interval);
    OverlayOptional(value.log_parameter, supplied.log_parameter);
    OverlayAxis(value.x_axis, supplied.x_axis);
    OverlayAxis(value.y_axis, supplied.y_axis);
}

void AppendUnique(std::vector<std::string>& destination, const std::vector<std::string>& values)
{
    for (const std::string& value : values)
    {
        if (!std::ranges::contains(destination, value))
        {
            destination.push_back(value);
        }
    }
}

Result<void> ValidateLocal(const UnresolvedDefinition& definition)
{
    if (definition.identity.xml_id.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} definition from '{}' has an empty definition identity",
                                                           FormatName(definition.format), definition.source));
    }
    if (definition.source.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} definition '{}' has no source",
                                                           FormatName(definition.format), definition.identity.xml_id));
    }

    std::vector<const UnresolvedCalibrationMap *> maps;
    for (const UnresolvedCalibrationMap& map : definition.maps)
    {
        const bool duplicate = std::ranges::any_of(maps, [&map](const UnresolvedCalibrationMap *candidate)
                                                   { return MapsMatch(*candidate, map); });
        if (const auto& key = MapKey(map); !key.empty() && duplicate)
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("duplicate map key '{}' in definition '{}' from '{}'",
                                                               key, definition.identity.xml_id, definition.source));
        }
        maps.push_back(&map);
    }

    std::unordered_map<std::string, const UnresolvedScaling *> scalings;
    for (const UnresolvedScaling& scaling : definition.scalings)
    {
        if (scaling.name.empty())
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("scaling with an empty name in definition '{}' from '{}'",
                                    definition.identity.xml_id, definition.source));
        }
        auto [existing, inserted] = scalings.try_emplace(scaling.name, &scaling);
        if (!inserted && *existing->second != scaling)
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("conflicting duplicate scaling '{}' in definition '{}' from '{}'", scaling.name,
                                    definition.identity.xml_id, definition.source));
        }
    }
    return {};
}

// Narrows `maps_match` candidates by id and name so merging does not rescan the whole
// map list per supplied map (that nested scan is quadratic in map count, which matters
// for definitions with thousands of maps). Matches found through the index are always
// re-verified against `maps_match` on the live map, so a stale bucket entry (e.g. after
// a merge changes a map's id or name) can only add a redundant candidate to check, never
// a false match.
class MapIndex
{
  public:
    void Add(const UnresolvedCalibrationMap& map, std::size_t index)
    {
        by_name_[map.name].push_back(index);
        if (const std::string *id = StableMapId(map))
        {
            by_id_[*id].push_back(index);
        }
    }

    std::vector<std::size_t> Candidates(const UnresolvedCalibrationMap& map) const
    {
        std::vector<std::size_t> result;
        if (const std::string *id = StableMapId(map))
        {
            AppendBucket(by_id_, *id, result);
        }
        AppendBucket(by_name_, map.name, result);
        std::ranges::sort(result);
        result.erase(std::ranges::unique(result).begin(), result.end());
        return result;
    }

  private:
    static void AppendBucket(const std::unordered_map<std::string, std::vector<std::size_t>>& buckets,
                             const std::string& key, std::vector<std::size_t>& result)
    {
        if (auto bucket = buckets.find(key); bucket != buckets.end())
        {
            result.insert(result.end(), bucket->second.begin(), bucket->second.end());
        }
    }

    std::unordered_map<std::string, std::vector<std::size_t>> by_id_;
    std::unordered_map<std::string, std::vector<std::size_t>> by_name_;
};

Result<void> OverlayDefinition(UnresolvedDefinition& value, const UnresolvedDefinition& supplied)
{
    value.format = supplied.format;
    value.source = supplied.source;
    OverlayIdentity(value.identity, supplied.identity);
    OverlayMetadata(value.metadata, supplied.metadata);
    value.parents = supplied.parents;

    MapIndex index;
    for (std::size_t i = 0; i < value.maps.size(); ++i)
    {
        index.Add(value.maps[i], i);
    }

    for (const UnresolvedCalibrationMap& map : supplied.maps)
    {
        std::optional<std::size_t> existing;
        for (std::size_t candidate : index.Candidates(map))
        {
            if (!MapsMatch(value.maps[candidate], map))
            {
                continue;
            }
            if (existing.has_value())
            {
                return Fail(ErrorKind::kInvalidConfig,
                            std::format("ambiguous map name fallback '{}' while resolving definition '{}' from '{}'",
                                        map.name, supplied.identity.xml_id, supplied.source));
            }
            existing = candidate;
        }
        if (!existing.has_value())
        {
            value.maps.push_back(map);
            index.Add(value.maps.back(), value.maps.size() - 1);
        }
        else
        {
            OverlayMap(value.maps[*existing], map);
            index.Add(value.maps[*existing], *existing);
        }
    }

    for (const UnresolvedScaling& scaling : supplied.scalings)
    {
        auto existing = std::ranges::find(value.scalings, scaling.name, &UnresolvedScaling::name);
        if (existing == std::ranges::end(value.scalings))
        {
            value.scalings.push_back(scaling);
        }
        else if (*existing != scaling)
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("conflicting duplicate scaling '{}' while resolving definition '{}' from '{}'",
                                    scaling.name, supplied.identity.xml_id, supplied.source));
        }
    }
    return {};
}

bool AxisIsPresent(const AxisDefinition& axis)
{
    return axis != AxisDefinition{};
}

AxisDefinition ResolveAxis(const UnresolvedAxisDefinition& value, std::uint32_t default_size)
{
    if (value == UnresolvedAxisDefinition{})
    {
        return {};
    }
    return AxisDefinition{
        .type = value.type,
        .name = value.name,
        .units = value.units,
        .format = value.format,
        .storage_type = value.storage_type,
        .endian = value.endian,
        .address = value.address,
        .size = value.size.value_or(default_size),
        .from_byte = value.from_byte.value_or("x"),
        .to_byte = value.to_byte.value_or("x"),
        .scaling_name = value.scaling_name,
        .start_position = value.start_position.value_or(1),
        .interval = value.interval.value_or(1),
        .log_parameter = value.log_parameter.value_or(""),
        .static_data = value.static_data.value_or(std::vector<std::string>{}),
    };
}

CalibrationMap ResolveMap(const UnresolvedCalibrationMap& value)
{
    return CalibrationMap{
        .id = value.id.value_or(value.name),
        .name = value.name,
        .type = value.type,
        .category = value.category,
        .subcategory = value.subcategory,
        .description = value.description,
        .address = value.address,
        .x_size = value.x_size.value_or(1),
        .y_size = value.y_size.value_or(1),
        .swap_xy = value.swap_xy.value_or(false),
        .flip_x = value.flip_x.value_or(false),
        .flip_y = value.flip_y.value_or(false),
        .level = value.level,
        .user_level = value.user_level,
        .scaling_name = value.scaling_name,
        .storage_type = value.storage_type,
        .endian = value.endian,
        .start_position = value.start_position.value_or(1),
        .interval = value.interval.value_or(1),
        .log_parameter = value.log_parameter.value_or(""),
        .x_axis = ResolveAxis(value.x_axis, value.x_size.value_or(1)),
        .y_axis = ResolveAxis(value.y_axis, value.y_size.value_or(1)),
    };
}

Result<void> ValidateScaling(const Scaling& scaling)
{
    if (scaling.selections.empty())
    {
        if (scaling.storage_type == StorageType::kBloblist)
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("scaling '{}' uses bloblist storage without selections", scaling.name));
        }
        return {};
    }
    if (scaling.storage_type != StorageType::kBloblist)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("scaling '{}' has selections but storage type is not bloblist", scaling.name));
    }

    std::unordered_set<std::string> names;
    const auto width = scaling.selections.front().value.size();
    for (const auto& [name, value] : scaling.selections)
    {
        if (name.empty() || value.empty())
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("scaling '{}' has an incomplete selection", scaling.name));
        }
        if (value.size() != width)
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("scaling '{}' has selection '{}' whose width differs from the "
                                    "first selection's",
                                    scaling.name, name));
        }
        if (!names.insert(name).second)
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("scaling '{}' has duplicate selection '{}'", scaling.name, name));
        }
    }
    return {};
}

Result<Scaling> ResolveScaling(const UnresolvedScaling& value)
{
    Scaling result{
        .name = value.name,
        .units = value.units,
        .from_byte = value.from_byte.value_or("x"),
        .to_byte = value.to_byte.value_or("x"),
        .format = value.format.value_or(""),
        .minimum = value.minimum,
        .maximum = value.maximum,
        .coarse_increment = value.coarse_increment,
        .fine_increment = value.fine_increment,
        .storage_type = value.storage_type,
        .endian = value.endian,
    };
    result.selections.reserve(value.selections.size());
    for (const auto& [name, hex] : value.selections)
    {
        auto bytes = ParseHexBytes(hex);
        if (!bytes.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("scaling '{}' has selection '{}' whose value is not whole hexadecimal bytes",
                                    value.name, name));
        }
        result.selections.push_back({.name = name, .value = std::move(*bytes)});
    }
    if (auto valid = ValidateScaling(result); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return result;
}

Result<RomDefinition> Materialize(const UnresolvedDefinition& value)
{
    RomDefinition result{
        .format = value.format,
        .source = value.source,
        .identity = value.identity,
        .metadata = value.metadata,
        .parents = value.parents,
    };
    result.maps.reserve(value.maps.size());
    std::ranges::transform(value.maps, std::back_inserter(result.maps), ResolveMap);
    result.scalings.reserve(value.scalings.size());
    for (const UnresolvedScaling& scaling : value.scalings)
    {
        auto resolved = ResolveScaling(scaling);
        if (!resolved.has_value())
        {
            return std::unexpected(resolved.error());
        }
        result.scalings.push_back(std::move(*resolved));
    }
    return result;
}

Result<void> ApplyAxisScaling(AxisDefinition& axis, std::string_view axis_context,
                              const std::unordered_map<std::string, const UnresolvedScaling *>& scalings)
{
    if (axis.scaling_name.empty())
    {
        return {};
    }
    auto scaling = scalings.find(axis.scaling_name);
    if (scaling == scalings.end())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("unresolved scaling '{}' for {}", axis.scaling_name, axis_context));
    }
    if (!scaling->second->selections.empty())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("{} cannot use selectable scaling '{}'", axis_context, axis.scaling_name));
    }
    if (axis.storage_type.has_value() && scaling->second->storage_type.has_value() &&
        axis.storage_type != scaling->second->storage_type)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("contradictory storage type for {} scaling '{}'", axis_context, axis.scaling_name));
    }
    if (!axis.endian.empty() && !scaling->second->endian.empty() && axis.endian != scaling->second->endian)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("contradictory endian for {} scaling '{}'", axis_context, axis.scaling_name));
    }

    OverlayString(axis.units, scaling->second->units);
    if (scaling->second->format || axis.format.empty())
    {
        axis.format = scaling->second->format.value_or("");
    }
    OverlayOptional(axis.storage_type, scaling->second->storage_type);
    OverlayString(axis.endian, scaling->second->endian);
    if (scaling->second->from_byte || axis.from_byte == "x")
    {
        axis.from_byte = scaling->second->from_byte.value_or("x");
    }
    if (scaling->second->to_byte || axis.to_byte == "x")
    {
        axis.to_byte = scaling->second->to_byte.value_or("x");
    }
    return {};
}

Result<void> ValidateAxis(AxisDefinition& axis, std::string_view axis_context, std::uint32_t required_size,
                          bool supports_static_data,
                          const std::unordered_map<std::string, const UnresolvedScaling *>& scalings)
{
    if (!AxisIsPresent(axis))
    {
        return {};
    }
    if (axis.type.empty() || (axis.name.empty() && axis.static_data.empty()))
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("incomplete {}", axis_context));
    }
    if (axis.size == 0)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("zero dimension for {}", axis_context));
    }
    if (axis.start_position == 0)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("zero start position for {}", axis_context));
    }
    if (axis.size != required_size)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("inconsistent dimension for {}", axis_context));
    }
    const bool is_static_axis = axis.type == "Static X Axis";
    if (is_static_axis && !supports_static_data)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("static data is not supported for {}", axis_context));
    }
    if (is_static_axis)
    {
        if (axis.static_data.size() != axis.size || std::ranges::any_of(axis.static_data, &std::string::empty))
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("static data count for {} does not match its size", axis_context));
        }
    }
    else if (!axis.static_data.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("static data on non-static {}", axis_context));
    }
    return ApplyAxisScaling(axis, axis_context, scalings);
}

Result<void> ValidateAndResolveMaps(RomDefinition& definition, const UnresolvedDefinition& unresolved)
{
    std::unordered_map<std::string, const UnresolvedScaling *> scalings;
    for (const UnresolvedScaling& scaling : unresolved.scalings)
    {
        scalings.try_emplace(scaling.name, &scaling);
    }

    std::vector<const CalibrationMap *> maps;
    for (CalibrationMap& map : definition.maps)
    {
        const std::string key = MapKey(map);
        if (key.empty() || map.name.empty())
        {
            return Fail(ErrorKind::kInvalidConfig, "incomplete map identity");
        }
        if (const bool duplicate = std::ranges::any_of(maps, [&map](const CalibrationMap *candidate)
                                                       { return MapsMatch(*candidate, map); });
            duplicate)
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("duplicate map key '{}'", key));
        }
        maps.push_back(&map);
        if (map.x_size == 0 || map.y_size == 0)
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("zero required dimension for map '{}'", key));
        }
        if (map.start_position == 0)
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("zero start position for map '{}'", key));
        }

        bool has_selection_scaling = false;
        if (!map.scaling_name.empty())
        {
            auto scaling = scalings.find(map.scaling_name);
            if (scaling == scalings.end())
            {
                return Fail(ErrorKind::kInvalidConfig,
                            std::format("unresolved scaling '{}' for map '{}'", map.scaling_name, key));
            }
            if (map.storage_type.has_value() && scaling->second->storage_type.has_value() &&
                map.storage_type != scaling->second->storage_type)
            {
                return Fail(
                    ErrorKind::kInvalidConfig,
                    std::format("contradictory storage type for map '{}' and scaling '{}'", key, map.scaling_name));
            }
            if (!map.endian.empty() && !scaling->second->endian.empty() && map.endian != scaling->second->endian)
            {
                return Fail(ErrorKind::kInvalidConfig,
                            std::format("contradictory endian for map '{}' and scaling '{}'", key, map.scaling_name));
            }
            OverlayOptional(map.storage_type, scaling->second->storage_type);
            OverlayString(map.endian, scaling->second->endian);
            if (!scaling->second->selections.empty())
            {
                map.type = "Selectable";
                has_selection_scaling = true;
            }
        }

        if (map.type == "Selectable" && (map.storage_type != StorageType::kBloblist || !has_selection_scaling))
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("selectable map '{}' requires a bloblist selection scaling", key));
        }
        if (map.storage_type == StorageType::kBloblist && map.type != "Selectable")
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("bloblist map '{}' must be selectable", key));
        }

        if (auto x_axis = ValidateAxis(map.x_axis, std::format("x axis for map '{}'", key), map.x_size, true, scalings);
            !x_axis.has_value())
        {
            return std::unexpected(x_axis.error());
        }

        if (auto y_axis =
                ValidateAxis(map.y_axis, std::format("y axis for map '{}'", key), map.y_size, false, scalings);
            !y_axis.has_value())
        {
            return std::unexpected(y_axis.error());
        }
    }
    return {};
}

std::string ChainText(const std::vector<std::string>& stack, std::string_view tail = {})
{
    std::string result;
    for (const std::string& id : stack)
    {
        if (!result.empty())
        {
            result += " -> ";
        }
        result += id;
    }
    if (!tail.empty())
    {
        if (!result.empty())
        {
            result += " -> ";
        }
        result += tail;
    }
    return result;
}

class ChainGuard
{
  public:
    ChainGuard(std::unordered_set<std::string>& visiting, std::vector<std::string>& stack, const std::string& id)
        : visiting_(visiting), stack_(stack), id_(id)
    {
    }

    ~ChainGuard()
    {
        stack_.pop_back();
        visiting_.erase(id_);
    }

  private:
    std::unordered_set<std::string>& visiting_;
    std::vector<std::string>& stack_;
    const std::string& id_;
};

class ResolverState
{
    struct Resolved
    {
        UnresolvedDefinition definition;
        std::vector<std::string> sources;
        std::vector<std::string> ids;
    };

  public:
    ResolverState(DefinitionFormat format, const DefinitionLoader& loader) : format_(format), loader_(loader)
    {
    }

    Result<RomDefinition> ResolveRoot(UnresolvedDefinition root)
    {
        const auto context =
            std::format("{} definitions '{}' from '{}': ", FormatName(root.format), root.identity.xml_id, root.source);
        auto resolved = Resolve(std::move(root));
        if (!resolved.has_value())
        {
            return Fail(resolved.error().kind, std::format("{}{}", context, resolved.error().detail));
        }
        auto materialized = Materialize(resolved->definition);
        if (!materialized.has_value())
        {
            return Fail(materialized.error().kind, std::format("{}{}", context, materialized.error().detail));
        }
        RomDefinition result = std::move(*materialized);
        result.resolved_sources = std::move(resolved->sources);
        result.resolved_definition_ids = std::move(resolved->ids);
        if (auto valid = ValidateAndResolveMaps(result, resolved->definition); !valid.has_value())
        {
            return Fail(valid.error().kind, std::format("{}{}", context, valid.error().detail));
        }
        return result;
    }

  private:
    Result<Resolved> Resolve(UnresolvedDefinition definition)
    {
        const std::string id = definition.identity.xml_id;
        if (id.empty())
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("{} definition from '{}' has an empty definition identity",
                                    FormatName(definition.format), definition.source));
        }
        if (visiting_.contains(id))
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("inheritance cycle: {}", ChainText(stack_, id)));
        }

        if (auto memoized = resolved_by_id_.find(id); memoized != resolved_by_id_.end())
        {
            return memoized->second;
        }

        if (stack_.size() >= kMaxInheritanceDepth)
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("inheritance chain exceeds maximum depth ({}): {}",
                                                               kMaxInheritanceDepth, ChainText(stack_, id)));
        }

        visiting_.insert(id);
        stack_.push_back(id);
        const ChainGuard guard{visiting_, stack_, id};

        if (auto locally_valid = ValidateLocal(definition); !locally_valid.has_value())
        {
            return Fail(locally_valid.error().kind,
                        std::format("{} in inheritance chain {}", locally_valid.error().detail, ChainText(stack_)));
        }

        Resolved resolved;
        bool has_parent = false;
        const std::vector<std::string> parent_ids = definition.parents;
        for (const std::string& parent_id : parent_ids)
        {
            if (visiting_.contains(parent_id))
            {
                return Fail(ErrorKind::kInvalidConfig,
                            std::format("inheritance cycle: {}", ChainText(stack_, parent_id)));
            }

            auto parent = resolved_by_id_.find(parent_id);
            if (parent == resolved_by_id_.end())
            {
                auto loaded = loader_(format_, parent_id);
                if (!loaded)
                {
                    return Fail(loaded.error().kind,
                                std::format("failed to load parent '{}' in inheritance chain {}: {}", parent_id,
                                            ChainText(stack_, parent_id), loaded.error().detail));
                }
                if (loaded->format != format_)
                {
                    return Fail(ErrorKind::kInvalidConfig,
                                std::format("cross-format parent '{}' in inheritance chain {}", parent_id,
                                            ChainText(stack_, parent_id)));
                }
                if (loaded->identity.xml_id != parent_id)
                {
                    return Fail(ErrorKind::kInvalidConfig,
                                std::format("parent reference '{}' loaded definition '{}' in inheritance chain {}",
                                            parent_id, loaded->identity.xml_id, ChainText(stack_, parent_id)));
                }

                if (auto parent_result = Resolve(std::move(*loaded)); !parent_result.has_value())
                {
                    return std::unexpected(parent_result.error());
                }
                parent = resolved_by_id_.find(parent_id);
            }

            if (!has_parent)
            {
                resolved = parent->second;
                has_parent = true;
            }
            else if (auto merged = OverlayDefinition(resolved.definition, parent->second.definition);
                     !merged.has_value())
            {
                return Fail(merged.error().kind,
                            std::format("{} in inheritance chain {}", merged.error().detail, ChainText(stack_)));
            }
            AppendUnique(resolved.sources, parent->second.sources);
            AppendUnique(resolved.ids, parent->second.ids);
        }

        if (auto merged = OverlayDefinition(resolved.definition, definition); !merged.has_value())
        {
            return Fail(merged.error().kind,
                        std::format("{} in inheritance chain {}", merged.error().detail, ChainText(stack_)));
        }
        AppendUnique(resolved.sources, {resolved.definition.source});
        AppendUnique(resolved.ids, {resolved.definition.identity.xml_id});

        auto [stored, inserted] = resolved_by_id_.try_emplace(id, std::move(resolved));
        return stored->second;
    }

    DefinitionFormat format_;
    const DefinitionLoader& loader_;
    std::unordered_set<std::string> visiting_;
    std::vector<std::string> stack_;
    std::unordered_map<std::string, Resolved> resolved_by_id_;
};

} // namespace

Result<RomDefinition> ResolveDefinition(UnresolvedDefinition root, const DefinitionLoader& loader)
{
    ResolverState state(root.format, loader);
    return state.ResolveRoot(std::move(root));
}

} // namespace fastecu::definition
