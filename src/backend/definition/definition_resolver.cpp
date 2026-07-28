#include "src/backend/definition/definition_resolver.h"

#include <algorithm>
#include <format>
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

bool has_stable_map_id(const CalibrationMap& map)
{
    return map.supplied.stable_id || (!map.supplied.tracked && !map.id.empty());
}

std::string map_key(const CalibrationMap& map)
{
    return has_stable_map_id(map) ? map.id : map.name;
}

bool maps_match(const CalibrationMap& left, const CalibrationMap& right)
{
    if (has_stable_map_id(left) && has_stable_map_id(right))
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

void overlay_axis(AxisDefinition& value, const AxisDefinition& supplied)
{
    if (supplied == AxisDefinition{})
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
    if (supplied.supplied.size ||
        (!supplied.supplied.tracked && supplied.size != AxisDefinition{}.size))
    {
        value.size = supplied.size;
        value.supplied.size = true;
    }
    if (supplied.supplied.from_byte ||
        (!supplied.supplied.tracked && supplied.from_byte != AxisDefinition{}.from_byte))
    {
        value.from_byte = supplied.from_byte;
        value.supplied.from_byte = true;
    }
    if (supplied.supplied.to_byte ||
        (!supplied.supplied.tracked && supplied.to_byte != AxisDefinition{}.to_byte))
    {
        value.to_byte = supplied.to_byte;
        value.supplied.to_byte = true;
    }
    overlay_string(value.scaling_name, supplied.scaling_name);
    if (supplied.supplied.start_position ||
        (!supplied.supplied.tracked &&
         supplied.start_position != AxisDefinition{}.start_position))
    {
        value.start_position = supplied.start_position;
        value.supplied.start_position = true;
    }
    if (supplied.supplied.interval ||
        (!supplied.supplied.tracked && supplied.interval != AxisDefinition{}.interval))
    {
        value.interval = supplied.interval;
        value.supplied.interval = true;
    }
    if (supplied.supplied.log_parameter ||
        (!supplied.supplied.tracked && !supplied.log_parameter.empty()))
    {
        value.log_parameter = supplied.log_parameter;
        value.supplied.log_parameter = true;
    }
    if (supplied.supplied.static_data ||
        (!supplied.supplied.tracked && !supplied.static_data.empty()))
    {
        value.static_data = supplied.static_data;
        value.supplied.static_data = true;
    }
}

void overlay_map(CalibrationMap& value, const CalibrationMap& supplied)
{
    if (has_stable_map_id(supplied))
    {
        value.id = supplied.id;
        value.supplied.stable_id = true;
    }
    overlay_string(value.name, supplied.name);
    overlay_string(value.type, supplied.type);
    overlay_string(value.category, supplied.category);
    overlay_string(value.subcategory, supplied.subcategory);
    overlay_string(value.description, supplied.description);
    overlay_optional(value.address, supplied.address);
    if (supplied.supplied.x_size ||
        (!supplied.supplied.tracked && supplied.x_size != CalibrationMap{}.x_size))
    {
        value.x_size = supplied.x_size;
        value.supplied.x_size = true;
    }
    if (supplied.supplied.y_size ||
        (!supplied.supplied.tracked && supplied.y_size != CalibrationMap{}.y_size))
    {
        value.y_size = supplied.y_size;
        value.supplied.y_size = true;
    }
    if (supplied.supplied.swap_xy || (!supplied.supplied.tracked && supplied.swap_xy))
    {
        value.swap_xy = supplied.swap_xy;
        value.supplied.swap_xy = true;
    }
    if (supplied.supplied.flip_x || (!supplied.supplied.tracked && supplied.flip_x))
    {
        value.flip_x = supplied.flip_x;
        value.supplied.flip_x = true;
    }
    if (supplied.supplied.flip_y || (!supplied.supplied.tracked && supplied.flip_y))
    {
        value.flip_y = supplied.flip_y;
        value.supplied.flip_y = true;
    }
    overlay_string(value.level, supplied.level);
    overlay_string(value.user_level, supplied.user_level);
    overlay_string(value.scaling_name, supplied.scaling_name);
    overlay_string(value.storage_type, supplied.storage_type);
    overlay_string(value.endian, supplied.endian);
    if (supplied.supplied.start_position ||
        (!supplied.supplied.tracked &&
         supplied.start_position != CalibrationMap{}.start_position))
    {
        value.start_position = supplied.start_position;
        value.supplied.start_position = true;
    }
    if (supplied.supplied.interval ||
        (!supplied.supplied.tracked && supplied.interval != CalibrationMap{}.interval))
    {
        value.interval = supplied.interval;
        value.supplied.interval = true;
    }
    if (supplied.supplied.log_parameter ||
        (!supplied.supplied.tracked && !supplied.log_parameter.empty()))
    {
        value.log_parameter = supplied.log_parameter;
        value.supplied.log_parameter = true;
    }
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

    std::vector<const CalibrationMap *> maps;
    for (const CalibrationMap& map : definition.maps)
    {
        const bool duplicate = std::ranges::any_of(
            maps,
            [&map](const CalibrationMap *candidate)
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

    std::unordered_map<std::string, const Scaling *> scalings;
    for (const Scaling& scaling : definition.scalings)
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

Result<void> overlay_definition(
    RomDefinition& value, const UnresolvedDefinition& supplied)
{
    value.format = supplied.format;
    value.source = supplied.source;
    overlay_identity(value.identity, supplied.identity);
    overlay_metadata(value.metadata, supplied.metadata);
    value.parents = supplied.parents;

    for (const CalibrationMap& map : supplied.maps)
    {
        CalibrationMap *existing = nullptr;
        for (CalibrationMap& candidate : value.maps)
        {
            if (!maps_match(candidate, map))
            {
                continue;
            }
            if (existing != nullptr)
            {
                return fail(
                    ErrorKind::InvalidConfig,
                    std::format("ambiguous map name fallback '{}' while resolving definition '{}' from '{}'", map.name, supplied.identity.xml_id, supplied.source));
            }
            existing = &candidate;
        }
        if (existing == nullptr)
        {
            value.maps.push_back(map);
        }
        else
        {
            overlay_map(*existing, map);
        }
    }

    for (const Scaling& scaling : supplied.scalings)
    {
        auto existing = std::ranges::find(
            value.scalings,
            scaling.name,
            &Scaling::name);
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
    const std::unordered_map<std::string, const Scaling *>& scalings,
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
    if (scaling->second->supplied.format ||
        (!scaling->second->supplied.tracked && !scaling->second->format.empty()) ||
        axis.format.empty())
    {
        axis.format = scaling->second->format;
    }
    overlay_string(axis.storage_type, scaling->second->storage_type);
    overlay_string(axis.endian, scaling->second->endian);
    const bool from_byte_supplied =
        scaling->second->supplied.from_byte ||
        (!scaling->second->supplied.tracked && !scaling->second->from_byte.empty());
    if (from_byte_supplied || !axis.supplied.from_byte)
    {
        axis.from_byte = scaling->second->from_byte;
        axis.supplied.from_byte = from_byte_supplied;
    }
    const bool to_byte_supplied =
        scaling->second->supplied.to_byte ||
        (!scaling->second->supplied.tracked && !scaling->second->to_byte.empty());
    if (to_byte_supplied || !axis.supplied.to_byte)
    {
        axis.to_byte = scaling->second->to_byte;
        axis.supplied.to_byte = to_byte_supplied;
    }
    return {};
}

Result<void> validate_axis(
    AxisDefinition& axis,
    std::string_view axis_context,
    std::uint32_t required_size,
    bool supports_static_data,
    const std::unordered_map<std::string, const Scaling *>& scalings,
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
    if (axis.supplied.tracked && !axis.supplied.size)
    {
        axis.size = required_size;
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

Result<void> validate_and_resolve_scalings(RomDefinition& definition)
{
    std::unordered_map<std::string, const Scaling *> scalings;
    for (const Scaling& scaling : definition.scalings)
    {
        if (const auto result = validate_scaling(scaling, definition.identity.xml_id); !result.has_value())
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

        auto x_axis = validate_axis(
            map.x_axis,
            "x axis for map '" + key + "'",
            map.x_size,
            true,
            scalings,
            definition.identity.xml_id);
        if (!x_axis)
        {
            return std::unexpected(x_axis.error());
        }
        auto y_axis = validate_axis(
            map.y_axis,
            "y axis for map '" + key + "'",
            map.y_size,
            false,
            scalings,
            definition.identity.xml_id);
        if (!y_axis)
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

class ResolverState
{
  public:
    ResolverState(DefinitionFormat format, const DefinitionLoader& loader)
        : format_(format), loader_(loader)
    {
    }

    Result<RomDefinition> resolve_root(UnresolvedDefinition root)
    {
        const std::string context =
            format_name(root.format) + " definition '" + root.identity.xml_id + "' from '" +
            root.source + "': ";
        auto resolved = resolve(std::move(root));
        if (!resolved)
        {
            return fail(resolved.error().kind, context + resolved.error().detail);
        }
        auto valid = validate_and_resolve_scalings(*resolved);
        if (!valid)
        {
            return fail(valid.error().kind, context + valid.error().detail);
        }
        return resolved;
    }

  private:
    Result<RomDefinition> resolve(UnresolvedDefinition definition)
    {
        const std::string id = definition.identity.xml_id;
        if (id.empty())
        {
            auto locally_valid = validate_local(definition);
            return std::unexpected(locally_valid.error());
        }
        if (visiting.contains(id))
        {
            return fail(
                ErrorKind::InvalidConfig,
                "inheritance cycle: " + chain_text(stack, id));
        }

        auto memoized = resolved_by_id.find(id);
        if (memoized != resolved_by_id.end())
        {
            return memoized->second;
        }

        visiting.insert(id);
        stack.push_back(id);

        auto locally_valid = validate_local(definition);
        if (!locally_valid)
        {
            return fail(
                locally_valid.error().kind,
                locally_valid.error().detail + " in inheritance chain " + chain_text(stack));
        }

        RomDefinition resolved;
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

                auto parent_result = resolve(std::move(*loaded));
                if (!parent_result)
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
            else
            {
                auto merged = overlay_definition(resolved, parent->second);
                if (!merged)
                {
                    return fail(
                        merged.error().kind,
                        merged.error().detail + " in inheritance chain " + chain_text(stack));
                }
            }
            append_unique(resolved.resolved_sources, parent->second.resolved_sources);
            append_unique(
                resolved.resolved_definition_ids,
                parent->second.resolved_definition_ids);
        }

        auto merged = overlay_definition(resolved, definition);
        if (!merged)
        {
            return fail(
                merged.error().kind,
                merged.error().detail + " in inheritance chain " + chain_text(stack));
        }
        append_unique(resolved.resolved_sources, {resolved.source});
        append_unique(
            resolved.resolved_definition_ids,
            {resolved.identity.xml_id});

        stack.pop_back();
        visiting.erase(id);
        auto [stored, inserted] = resolved_by_id.try_emplace(id, std::move(resolved));
        (void)inserted;
        return stored->second;
    }

    DefinitionFormat format_;
    const DefinitionLoader& loader_;
    std::unordered_set<std::string> visiting;
    std::vector<std::string> stack;
    std::unordered_map<std::string, RomDefinition> resolved_by_id;
};

} // namespace

Result<RomDefinition> resolve_definition(
    UnresolvedDefinition root, const DefinitionLoader& loader)
{
    ResolverState state(root.format, loader);
    return state.resolve_root(std::move(root));
}

} // namespace fastecu::definition
