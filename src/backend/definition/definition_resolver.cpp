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

namespace fastecu::definition
{
namespace
{

std::string format_name(DefinitionFormat format)
{
    return format == DefinitionFormat::RomRaider ? "RomRaider" : "EcuFlash";
}

bool has_stable_map_id(const UnresolvedCalibrationMap& map)
{
    return map.id.has_value() && !map.id->empty();
}

std::string map_key(const UnresolvedCalibrationMap& map)
{
    return has_stable_map_id(map) ? *map.id : map.name;
}

bool maps_match(const UnresolvedCalibrationMap& left, const UnresolvedCalibrationMap& right)
{
    if (has_stable_map_id(left) && has_stable_map_id(right))
    {
        return left.id == right.id;
    }
    return left.name == right.name;
}

std::string map_key(const CalibrationMap& map)
{
    return !map.id.empty() ? map.id : map.name;
}

bool maps_match(const CalibrationMap& left, const CalibrationMap& right)
{
    if (!left.id.empty() && !right.id.empty())
    {
        return left.id == right.id;
    }
    return left.name == right.name;
}

void overlay_string(std::string& value, std::string_view supplied)
{
    if (!supplied.empty())
    {
        value = supplied;
    }
}

template <typename Value>
void overlay_optional(Value& value, const Value& supplied)
{
    if (supplied)
    {
        value = supplied;
    }
}

void overlay_identity(RomIdentity& value, const RomIdentity& supplied)
{
    value.xml_id = supplied.xml_id;
    overlay_string(value.internal_id, supplied.internal_id);
    overlay_string(value.ecu_id, supplied.ecu_id);
    overlay_optional(value.internal_id_address, supplied.internal_id_address);
}

void overlay_metadata(RomMetadata& value, const RomMetadata& supplied)
{
    overlay_string(value.make, supplied.make);
    overlay_string(value.market, supplied.market);
    overlay_string(value.model, supplied.model);
    overlay_string(value.submodel, supplied.submodel);
    overlay_string(value.transmission, supplied.transmission);
    overlay_string(value.year, supplied.year);
    overlay_string(value.flash_method, supplied.flash_method);
    overlay_string(value.memory_model, supplied.memory_model);
    overlay_string(value.checksum_module, supplied.checksum_module);
    overlay_string(value.file_size, supplied.file_size);
    overlay_string(value.notes, supplied.notes);
}

void overlay_axis(UnresolvedAxisDefinition& value, const UnresolvedAxisDefinition& supplied)
{
    if (supplied == UnresolvedAxisDefinition{})
    {
        return;
    }
    overlay_string(value.type, supplied.type);
    overlay_string(value.name, supplied.name);
    overlay_string(value.units, supplied.units);
    overlay_string(value.format, supplied.format);
    overlay_string(value.storage_type, supplied.storage_type);
    overlay_string(value.endian, supplied.endian);
    overlay_optional(value.address, supplied.address);
    overlay_optional(value.size, supplied.size);
    overlay_optional(value.from_byte, supplied.from_byte);
    overlay_optional(value.to_byte, supplied.to_byte);
    overlay_string(value.scaling_name, supplied.scaling_name);
    overlay_optional(value.start_position, supplied.start_position);
    overlay_optional(value.interval, supplied.interval);
    overlay_optional(value.log_parameter, supplied.log_parameter);
    overlay_optional(value.static_data, supplied.static_data);
}

void overlay_map(UnresolvedCalibrationMap& value, const UnresolvedCalibrationMap& supplied)
{
    if (has_stable_map_id(supplied))
    {
        value.id = supplied.id;
    }
    overlay_string(value.name, supplied.name);
    overlay_string(value.type, supplied.type);
    overlay_string(value.category, supplied.category);
    overlay_string(value.subcategory, supplied.subcategory);
    overlay_string(value.description, supplied.description);
    overlay_optional(value.address, supplied.address);
    overlay_optional(value.x_size, supplied.x_size);
    overlay_optional(value.y_size, supplied.y_size);
    overlay_optional(value.swap_xy, supplied.swap_xy);
    overlay_optional(value.flip_x, supplied.flip_x);
    overlay_optional(value.flip_y, supplied.flip_y);
    overlay_string(value.level, supplied.level);
    overlay_string(value.user_level, supplied.user_level);
    overlay_string(value.scaling_name, supplied.scaling_name);
    overlay_string(value.storage_type, supplied.storage_type);
    overlay_string(value.endian, supplied.endian);
    overlay_optional(value.start_position, supplied.start_position);
    overlay_optional(value.interval, supplied.interval);
    overlay_optional(value.log_parameter, supplied.log_parameter);
    overlay_axis(value.x_axis, supplied.x_axis);
    overlay_axis(value.y_axis, supplied.y_axis);
}

void append_unique(std::vector<std::string>& destination, const std::vector<std::string>& values)
{
    for (const std::string& value : values)
    {
        if (!std::ranges::contains(destination, value))
        {
            destination.push_back(value);
        }
    }
}

Result<void> validate_local(const UnresolvedDefinition& definition)
{
    if (definition.identity.xml_id.empty())
    {
        return fail(
            ErrorKind::InvalidConfig,
            std::format("{} definition from '{}' has an empty definition identity", format_name(definition.format), definition.source));
    }
    if (definition.source.empty())
    {
        return fail(
            ErrorKind::InvalidConfig,
            std::format("{} definition '{}' has no source", format_name(definition.format), definition.identity.xml_id));
    }

    std::vector<const UnresolvedCalibrationMap *> maps;
    for (const UnresolvedCalibrationMap& map : definition.maps)
    {
        const bool duplicate = std::ranges::any_of(
            maps,
            [&map](const UnresolvedCalibrationMap *candidate)
            {
                return maps_match(*candidate, map);
            });
        if (const auto& key = map_key(map); !key.empty() && duplicate)
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format("duplicate map key '{}' in definition '{}' from '{}'", key, definition.identity.xml_id, definition.source));
        }
        maps.push_back(&map);
    }

    std::unordered_map<std::string, const UnresolvedScaling *> scalings;
    for (const UnresolvedScaling& scaling : definition.scalings)
    {
        if (scaling.name.empty())
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format("scaling with an empty name in definition '{}' from '{}'", definition.identity.xml_id, definition.source));
        }
        auto [existing, inserted] = scalings.try_emplace(scaling.name, &scaling);
        if (!inserted && *existing->second != scaling)
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format("conflicting duplicate scaling '{}' in definition '{}' from '{}'", scaling.name, definition.identity.xml_id, definition.source));
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
    void add(const UnresolvedCalibrationMap& map, std::size_t index)
    {
        by_name_[map.name].push_back(index);
        if (has_stable_map_id(map))
        {
            by_id_[*map.id].push_back(index);
        }
    }

    std::vector<std::size_t> candidates(const UnresolvedCalibrationMap& map) const
    {
        std::vector<std::size_t> result;
        if (has_stable_map_id(map))
        {
            append_bucket(by_id_, *map.id, result);
        }
        append_bucket(by_name_, map.name, result);
        std::ranges::sort(result);
        result.erase(std::ranges::unique(result).begin(), result.end());
        return result;
    }

  private:
    static void append_bucket(
        const std::unordered_map<std::string, std::vector<std::size_t>>& buckets,
        const std::string& key,
        std::vector<std::size_t>& result)
    {
        if (auto bucket = buckets.find(key); bucket != buckets.end())
        {
            result.insert(result.end(), bucket->second.begin(), bucket->second.end());
        }
    }

    std::unordered_map<std::string, std::vector<std::size_t>> by_id_;
    std::unordered_map<std::string, std::vector<std::size_t>> by_name_;
};

Result<void> overlay_definition(
    UnresolvedDefinition& value, const UnresolvedDefinition& supplied)
{
    value.format = supplied.format;
    value.source = supplied.source;
    overlay_identity(value.identity, supplied.identity);
    overlay_metadata(value.metadata, supplied.metadata);
    value.parents = supplied.parents;

    MapIndex index;
    for (std::size_t i = 0; i < value.maps.size(); ++i)
    {
        index.add(value.maps[i], i);
    }

    for (const UnresolvedCalibrationMap& map : supplied.maps)
    {
        std::optional<std::size_t> existing;
        for (std::size_t candidate : index.candidates(map))
        {
            if (!maps_match(value.maps[candidate], map))
            {
                continue;
            }
            if (existing.has_value())
            {
                return fail(
                    ErrorKind::InvalidConfig,
                    std::format("ambiguous map name fallback '{}' while resolving definition '{}' from '{}'", map.name, supplied.identity.xml_id, supplied.source));
            }
            existing = candidate;
        }
        if (!existing.has_value())
        {
            value.maps.push_back(map);
            index.add(value.maps.back(), value.maps.size() - 1);
        }
        else
        {
            overlay_map(value.maps[*existing], map);
            index.add(value.maps[*existing], *existing);
        }
    }

    for (const UnresolvedScaling& scaling : supplied.scalings)
    {
        auto existing = std::ranges::find(
            value.scalings,
            scaling.name,
            &UnresolvedScaling::name);
        if (existing == std::ranges::end(value.scalings))
        {
            value.scalings.push_back(scaling);
        }
        else if (*existing != scaling)
        {
            return fail(
                ErrorKind::InvalidConfig,
                "conflicting duplicate scaling '" + scaling.name + "' while resolving definition '" +
                    supplied.identity.xml_id + "' from '" + supplied.source + "'");
        }
    }
    return {};
}

bool axis_is_present(const AxisDefinition& axis)
{
    return axis != AxisDefinition{};
}

Scaling resolve_scaling(const UnresolvedScaling& value)
{
    return Scaling{
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
        .selections = value.selections,
    };
}

AxisDefinition resolve_axis(
    const UnresolvedAxisDefinition& value, std::uint32_t default_size)
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
        .start_position = value.start_position.value_or("1"),
        .interval = value.interval.value_or("1"),
        .log_parameter = value.log_parameter.value_or(""),
        .static_data = value.static_data.value_or(std::vector<std::string>{}),
    };
}

CalibrationMap resolve_map(const UnresolvedCalibrationMap& value)
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
        .start_position = value.start_position.value_or("1"),
        .interval = value.interval.value_or("1"),
        .log_parameter = value.log_parameter.value_or(""),
        .x_axis = resolve_axis(value.x_axis, value.x_size.value_or(1)),
        .y_axis = resolve_axis(value.y_axis, value.y_size.value_or(1)),
    };
}

RomDefinition materialize(const UnresolvedDefinition& value)
{
    RomDefinition result{
        .format = value.format,
        .source = value.source,
        .identity = value.identity,
        .metadata = value.metadata,
        .parents = value.parents,
    };
    result.maps.reserve(value.maps.size());
    std::ranges::transform(value.maps, std::back_inserter(result.maps), resolve_map);
    result.scalings.reserve(value.scalings.size());
    std::ranges::transform(
        value.scalings, std::back_inserter(result.scalings), resolve_scaling);
    return result;
}

Result<void> validate_scaling(const Scaling& scaling, std::string_view definition_id)
{
    if (scaling.selections.empty())
    {
        if (scaling.storage_type == "bloblist")
        {
            return fail(
                ErrorKind::InvalidConfig,
                "scaling '" + scaling.name + "' in definition '" + std::string(definition_id) +
                    "' uses bloblist storage without selections");
        }
        return {};
    }
    if (scaling.storage_type != "bloblist")
    {
        return fail(
            ErrorKind::InvalidConfig,
            "scaling '" + scaling.name + "' in definition '" + std::string(definition_id) +
                "' has selections but storage type is not bloblist");
    }

    std::unordered_set<std::string> names;
    for (const auto& [name, value] : scaling.selections)
    {
        if (name.empty() || value.empty())
        {
            return fail(
                ErrorKind::InvalidConfig,
                "scaling '" + scaling.name + "' in definition '" +
                    std::string(definition_id) + "' has an incomplete selection");
        }
        if (!names.insert(name).second)
        {
            return fail(
                ErrorKind::InvalidConfig,
                "scaling '" + scaling.name + "' in definition '" +
                    std::string(definition_id) + "' has duplicate selection '" + name + "'");
        }
    }
    return {};
}

Result<void> apply_axis_scaling(
    AxisDefinition& axis,
    std::string_view axis_context,
    const std::unordered_map<std::string, const UnresolvedScaling *>& scalings,
    std::string_view definition_id)
{
    if (axis.scaling_name.empty())
    {
        return {};
    }
    auto scaling = scalings.find(axis.scaling_name);
    if (scaling == scalings.end())
    {
        return fail(
            ErrorKind::InvalidConfig,
            "unresolved scaling '" + axis.scaling_name + "' for " + std::string(axis_context) +
                " in definition '" + std::string(definition_id) + "'");
    }
    if (!scaling->second->selections.empty())
    {
        return fail(
            ErrorKind::InvalidConfig,
            std::string(axis_context) + " in definition '" + std::string(definition_id) +
                "' cannot use selectable scaling '" + axis.scaling_name + "'");
    }
    if (!axis.storage_type.empty() && !scaling->second->storage_type.empty() &&
        axis.storage_type != scaling->second->storage_type)
    {
        return fail(
            ErrorKind::InvalidConfig,
            "contradictory storage type for " + std::string(axis_context) + " scaling '" +
                axis.scaling_name + "' in definition '" + std::string(definition_id) + "'");
    }
    if (!axis.endian.empty() && !scaling->second->endian.empty() &&
        axis.endian != scaling->second->endian)
    {
        return fail(
            ErrorKind::InvalidConfig,
            "contradictory endian for " + std::string(axis_context) + " scaling '" +
                axis.scaling_name + "' in definition '" + std::string(definition_id) + "'");
    }

    overlay_string(axis.units, scaling->second->units);
    if (scaling->second->format || axis.format.empty())
    {
        axis.format = scaling->second->format.value_or("");
    }
    overlay_string(axis.storage_type, scaling->second->storage_type);
    overlay_string(axis.endian, scaling->second->endian);
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

Result<void> validate_axis(
    AxisDefinition& axis,
    std::string_view axis_context,
    std::uint32_t required_size,
    bool supports_static_data,
    const std::unordered_map<std::string, const UnresolvedScaling *>& scalings,
    std::string_view definition_id)
{
    if (!axis_is_present(axis))
    {
        return {};
    }
    if (axis.type.empty() ||
        (axis.name.empty() && axis.static_data.empty()))
    {
        return fail(
            ErrorKind::InvalidConfig,
            "incomplete " + std::string(axis_context) + " in definition '" +
                std::string(definition_id) + "'");
    }
    if (axis.size == 0)
    {
        return fail(
            ErrorKind::InvalidConfig,
            "zero dimension for " + std::string(axis_context) + " in definition '" +
                std::string(definition_id) + "'");
    }
    if (axis.size != required_size)
    {
        return fail(
            ErrorKind::InvalidConfig,
            "inconsistent dimension for " + std::string(axis_context) + " in definition '" +
                std::string(definition_id) + "'");
    }
    const bool is_static_axis = axis.type == "Static X Axis";
    if (is_static_axis && !supports_static_data)
    {
        return fail(
            ErrorKind::InvalidConfig,
            "static data is not supported for " + std::string(axis_context) +
                " in definition '" + std::string(definition_id) + "'");
    }
    if (is_static_axis)
    {
        if (axis.static_data.size() != axis.size ||
            std::ranges::any_of(
                axis.static_data, &std::string::empty))
        {
            return fail(
                ErrorKind::InvalidConfig,
                "static data count for " + std::string(axis_context) +
                    " does not match its size in definition '" +
                    std::string(definition_id) + "'");
        }
    }
    else if (!axis.static_data.empty())
    {
        return fail(
            ErrorKind::InvalidConfig,
            "static data on non-static " + std::string(axis_context) +
                " in definition '" + std::string(definition_id) + "'");
    }
    return apply_axis_scaling(axis, axis_context, scalings, definition_id);
}

Result<void> validate_and_resolve_scalings(
    RomDefinition& definition, const UnresolvedDefinition& unresolved)
{
    std::unordered_map<std::string, const UnresolvedScaling *> scalings;
    for (const UnresolvedScaling& scaling : unresolved.scalings)
    {
        if (const auto result =
                validate_scaling(resolve_scaling(scaling), definition.identity.xml_id);
            !result.has_value())
        {
            return std::unexpected(result.error());
        }
        scalings.try_emplace(scaling.name, &scaling);
    }

    std::vector<const CalibrationMap *> maps;
    for (CalibrationMap& map : definition.maps)
    {
        const std::string key = map_key(map);
        if (key.empty() || map.name.empty())
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format("incomplete map identity in definition '{}'", definition.identity.xml_id));
        }
        const bool duplicate = std::ranges::any_of(
            maps,
            [&map](const CalibrationMap *candidate)
            {
                return maps_match(*candidate, map);
            });
        if (duplicate)
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format("duplicate map key '{}' in resolved definition '{}'", key, definition.identity.xml_id));
        }
        maps.push_back(&map);
        if (map.x_size == 0 || map.y_size == 0)
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format("zero required dimension for map '{}' in definition '{}'", key, definition.identity.xml_id));
        }

        bool has_selection_scaling = false;
        if (!map.scaling_name.empty())
        {
            auto scaling = scalings.find(map.scaling_name);
            if (scaling == scalings.end())
            {
                return fail(
                    ErrorKind::InvalidConfig,
                    "unresolved scaling '" + map.scaling_name + "' for map '" + key +
                        "' in definition '" + definition.identity.xml_id + "'");
            }
            if (!map.storage_type.empty() && !scaling->second->storage_type.empty() &&
                map.storage_type != scaling->second->storage_type)
            {
                return fail(
                    ErrorKind::InvalidConfig,
                    "contradictory storage type for map '" + key + "' and scaling '" +
                        map.scaling_name + "' in definition '" + definition.identity.xml_id + "'");
            }
            if (!map.endian.empty() && !scaling->second->endian.empty() &&
                map.endian != scaling->second->endian)
            {
                return fail(
                    ErrorKind::InvalidConfig,
                    "contradictory endian for map '" + key + "' and scaling '" +
                        map.scaling_name + "' in definition '" + definition.identity.xml_id + "'");
            }
            overlay_string(map.storage_type, scaling->second->storage_type);
            overlay_string(map.endian, scaling->second->endian);
            if (!scaling->second->selections.empty())
            {
                map.type = "Selectable";
                has_selection_scaling = true;
            }
        }

        if (map.type == "Selectable" &&
            (map.storage_type != "bloblist" || !has_selection_scaling))
        {
            return fail(
                ErrorKind::InvalidConfig,
                "selectable map '" + key + "' in definition '" + definition.identity.xml_id +
                    "' requires a bloblist selection scaling");
        }
        if (map.storage_type == "bloblist" && map.type != "Selectable")
        {
            return fail(
                ErrorKind::InvalidConfig,
                "bloblist map '" + key + "' in definition '" + definition.identity.xml_id +
                    "' must be selectable");
        }

        if (auto x_axis = validate_axis(
                map.x_axis,
                std::format("x axis for map '{}'", key),
                map.x_size,
                true,
                scalings,
                definition.identity.xml_id);
            !x_axis.has_value())
        {
            return std::unexpected(x_axis.error());
        }

        if (auto y_axis = validate_axis(
                map.y_axis,
                std::format("y axis for map '{}'", key),
                map.y_size,
                false,
                scalings,
                definition.identity.xml_id);
            !y_axis.has_value())
        {
            return std::unexpected(y_axis.error());
        }
    }
    return {};
}

std::string chain_text(const std::vector<std::string>& stack, std::string_view tail = {})
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

// Real definition families are at most a handful of levels deep; this is a generous ceiling
// that still fails fast with a resolvable error well short of the C++ call stack limit for
// `ResolverState::resolve`'s recursion.
constexpr std::size_t kMaxInheritanceDepth = 256;

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
    ResolverState(DefinitionFormat format, const DefinitionLoader& loader)
        : format_(format), loader_(loader)
    {
    }

    Result<RomDefinition> resolve_root(UnresolvedDefinition root)
    {
        const auto context = std::format("{} definitions '{}' from '{}': ", format_name(root.format), root.identity.xml_id, root.source);
        auto resolved = resolve(std::move(root));
        if (!resolved.has_value())
        {
            return fail(resolved.error().kind, context + resolved.error().detail);
        }
        RomDefinition result = materialize(resolved->definition);
        result.resolved_sources = std::move(resolved->sources);
        result.resolved_definition_ids = std::move(resolved->ids);
        if (auto valid =
                validate_and_resolve_scalings(result, resolved->definition);
            !valid.has_value())
        {
            return fail(valid.error().kind, context + valid.error().detail);
        }
        return result;
    }

  private:
    Result<Resolved> resolve(UnresolvedDefinition definition)
    {
        const std::string id = definition.identity.xml_id;
        if (id.empty())
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format("{} definition from '{}' has an empty definition identity", format_name(definition.format), definition.source));
        }
        if (visiting.contains(id))
        {
            return fail(
                ErrorKind::InvalidConfig,
                "inheritance cycle: " + chain_text(stack, id));
        }

        if (auto memoized = resolved_by_id.find(id); memoized != resolved_by_id.end())
        {
            return memoized->second;
        }

        if (stack.size() >= kMaxInheritanceDepth)
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format(
                    "inheritance chain exceeds maximum depth ({}): {}",
                    kMaxInheritanceDepth,
                    chain_text(stack, id)));
        }

        visiting.insert(id);
        stack.push_back(id);
        const ChainGuard guard{visiting, stack, id};

        if (auto locally_valid = validate_local(definition); !locally_valid.has_value())
        {
            return fail(
                locally_valid.error().kind,
                std::format("{} in inheritance chain {}", locally_valid.error().detail, chain_text(stack)));
        }

        Resolved resolved;
        bool has_parent = false;
        const std::vector<std::string> parent_ids = definition.parents;
        for (const std::string& parent_id : parent_ids)
        {
            if (visiting.contains(parent_id))
            {
                return fail(
                    ErrorKind::InvalidConfig,
                    "inheritance cycle: " + chain_text(stack, parent_id));
            }

            auto parent = resolved_by_id.find(parent_id);
            if (parent == resolved_by_id.end())
            {
                auto loaded = loader_(format_, parent_id);
                if (!loaded)
                {
                    return fail(
                        loaded.error().kind,
                        "failed to load parent '" + parent_id + "' in inheritance chain " +
                            chain_text(stack, parent_id) + ": " + loaded.error().detail);
                }
                if (loaded->format != format_)
                {
                    return fail(
                        ErrorKind::InvalidConfig,
                        "cross-format parent '" + parent_id + "' in inheritance chain " +
                            chain_text(stack, parent_id));
                }
                if (loaded->identity.xml_id != parent_id)
                {
                    return fail(
                        ErrorKind::InvalidConfig,
                        "parent reference '" + parent_id + "' loaded definition '" +
                            loaded->identity.xml_id + "' in inheritance chain " +
                            chain_text(stack, parent_id));
                }

                if (auto parent_result = resolve(std::move(*loaded)); !parent_result.has_value())
                {
                    return std::unexpected(parent_result.error());
                }
                parent = resolved_by_id.find(parent_id);
            }

            if (!has_parent)
            {
                resolved = parent->second;
                has_parent = true;
            }
            else if (auto merged = overlay_definition(
                         resolved.definition, parent->second.definition);
                     !merged.has_value())
            {
                return fail(
                    merged.error().kind,
                    merged.error().detail + " in inheritance chain " + chain_text(stack));
            }
            append_unique(resolved.sources, parent->second.sources);
            append_unique(resolved.ids, parent->second.ids);
        }

        if (auto merged = overlay_definition(resolved.definition, definition); !merged.has_value())
        {
            return fail(
                merged.error().kind,
                merged.error().detail + " in inheritance chain " + chain_text(stack));
        }
        append_unique(resolved.sources, {resolved.definition.source});
        append_unique(resolved.ids, {resolved.definition.identity.xml_id});

        auto [stored, inserted] = resolved_by_id.try_emplace(id, std::move(resolved));
        return stored->second;
    }

    DefinitionFormat format_;
    const DefinitionLoader& loader_;
    std::unordered_set<std::string> visiting;
    std::vector<std::string> stack;
    std::unordered_map<std::string, Resolved> resolved_by_id;
};

} // namespace

Result<RomDefinition> resolve_definition(
    UnresolvedDefinition root, const DefinitionLoader& loader)
{
    ResolverState state(root.format, loader);
    return state.resolve_root(std::move(root));
}

} // namespace fastecu::definition
