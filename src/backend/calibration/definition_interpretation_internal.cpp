#include "src/backend/calibration/definition_interpretation_internal.h"

#include <limits>
#include <string_view>

#include "src/algorithms/expression/expression.h"

namespace fastecu::calibration::internal
{
namespace
{
double IncrementValue(std::string_view text)
{
    if (text.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos)
    {
        return 0.0;
    }
    const auto value = expression::ParseFiniteNumber(text);
    // Increment metadata cannot prevent an operation that does not use it.
    return value.has_value() ? *value : std::numeric_limits<double>::quiet_NaN();
}
} // namespace

MapElementSpec ResolvedElement::EditSpec() const&
{
    return {.address = address.has_value() ? address->Value() : 0,
            .storage_type = storage_type,
            .endian = endian,
            .to_byte = to_byte,
            .from_byte = from_byte,
            .min_value = min_value,
            .max_value = max_value,
            .coarse_increment = coarse_increment,
            .fine_increment = fine_increment,
            .x_size = dimensions.x_size,
            .y_size = dimensions.y_size,
            .start_position = start_position,
            .interval = interval};
}

std::uint32_t ElementByteWidth(std::optional<definition::StorageType> storage_type, const definition::Scaling *scaling)
{
    if (storage_type == definition::StorageType::kBloblist && scaling != nullptr && !scaling->selections.empty())
    {
        return static_cast<std::uint32_t>(scaling->selections.front().value.size());
    }
    return definition::StorageByteSize(storage_type);
}

ResolvedElement ResolveMapElement(const definition::RomDefinition& definition, const definition::CalibrationMap& map,
                                  NumericTarget target)
{
    ResolvedElement result;
    result.dimensions = {.x_size = map.x_size, .y_size = map.y_size};
    const definition::Scaling *scaling = nullptr;
    if (target == NumericTarget::kMapBody)
    {
        scaling = definition::FindScaling(definition, map.scaling_name);
        result.address = map.address;
        result.storage_type = map.storage_type.has_value() ? map.storage_type
                              : scaling != nullptr         ? scaling->storage_type
                                                           : std::nullopt;
        result.endian = !map.endian.empty() ? map.endian : scaling != nullptr ? scaling->endian : "";
        result.from_byte = scaling != nullptr ? scaling->from_byte : "x";
        result.to_byte = scaling != nullptr ? scaling->to_byte : "x";
        result.width = map.x_size;
        result.height = map.y_size;
        result.start_position = map.start_position;
        result.interval = map.interval;
        result.is_selectable = map.type == "Selectable";
    }
    else
    {
        const auto& axis = target == NumericTarget::kXAxis ? map.x_axis : map.y_axis;
        scaling = definition::FindScaling(definition, axis.scaling_name);
        result.address = axis.address;
        result.storage_type = axis.storage_type;
        result.endian = axis.endian;
        result.from_byte = axis.from_byte;
        result.to_byte = axis.to_byte;
        result.width = target == NumericTarget::kXAxis ? map.x_size : 1;
        result.height = target == NumericTarget::kYAxis ? map.y_size : 1;
        result.start_position = axis.start_position;
        result.interval = axis.interval;
        result.is_selectable = axis.type == "Selectable";
    }
    result.count = std::uint64_t{result.width} * result.height;
    result.byte_width = ElementByteWidth(result.storage_type, scaling);
    if (scaling != nullptr)
    {
        result.min_value = scaling->minimum;
        result.max_value = scaling->maximum;
        result.coarse_increment = IncrementValue(scaling->coarse_increment);
        result.fine_increment = IncrementValue(scaling->fine_increment);
    }
    return result;
}
} // namespace fastecu::calibration::internal
