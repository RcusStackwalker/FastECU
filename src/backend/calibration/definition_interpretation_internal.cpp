#include "src/backend/calibration/definition_interpretation_internal.h"

#include <algorithm>
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

std::uint32_t ElementByteWidth(std::optional<definition::StorageType> storage_type, const definition::Scaling *scaling)
{
    if (storage_type == definition::StorageType::kBloblist && scaling != nullptr && !scaling->selections.empty())
    {
        return static_cast<std::uint32_t>(scaling->selections.front().value.size());
    }
    return definition::StorageByteSize(storage_type);
}
// A body's storage is its own, else its scaling's.
std::optional<definition::StorageType> BodyStorage(const definition::CalibrationMap& map,
                                                   const definition::Scaling *scaling)
{
    return map.storage_type.has_value() ? map.storage_type : scaling != nullptr ? scaling->storage_type : std::nullopt;
}
} // namespace

ElementRun MapElement::Run() const&
{
    return {.address = address.has_value() ? address->Value() : 0,
            .count = static_cast<std::uint32_t>(Count()),
            .start_position = start_position,
            .interval = interval,
            .storage_type = storage_type,
            .endian = endian_,
            .from_byte = from_byte_,
            .is_selectable = is_selectable_};
}

MapElementSpec MapElement::EditSpec() const&
{
    return {.address = address.has_value() ? address->Value() : 0,
            .storage_type = storage_type,
            .endian = endian_,
            .to_byte = to_byte_,
            .from_byte = from_byte_,
            .min_value = min_value_,
            .max_value = max_value_,
            .coarse_increment = coarse_increment_,
            .fine_increment = fine_increment_,
            .x_size = dimensions_.x_size,
            .y_size = dimensions_.y_size,
            .start_position = start_position,
            .interval = interval};
}

Result<std::variant<BlobPatch, BlobMiss>> ResolveBlobSelection(const definition::RomDefinition& definition,
                                                               const definition::CalibrationMap& map,
                                                               std::string_view selection)
{
    using Resolved = std::variant<BlobPatch, BlobMiss>;
    const auto *scaling = definition::FindScaling(definition, map.scaling_name);
    const auto storage = BodyStorage(map, scaling);
    if (scaling == nullptr || scaling->selections.empty() || storage != definition::StorageType::kBloblist)
    {
        return Resolved{BlobMiss::kNotABlob};
    }
    const auto selected = std::ranges::find(scaling->selections, selection, &definition::Selection::name);
    if (selected == scaling->selections.end())
    {
        return Resolved{BlobMiss::kUnknownSelection};
    }
    if (selected->value.size() != ElementByteWidth(storage, scaling))
    {
        return Fail(ErrorKind::kInvalidConfig, "selection value width differs from the blob width");
    }
    return Resolved{BlobPatch{.address = map.address.has_value() ? map.address->Value() : 0, .bytes = selected->value}};
}

MapElement ResolveMapElement(const definition::RomDefinition& definition, const definition::CalibrationMap& map,
                             NumericTarget target)
{
    MapElement result;
    result.dimensions_ = {.x_size = map.x_size, .y_size = map.y_size};
    const definition::Scaling *scaling = nullptr;
    if (target == NumericTarget::kMapBody)
    {
        scaling = definition::FindScaling(definition, map.scaling_name);
        result.address = map.address;
        result.storage_type = BodyStorage(map, scaling);
        result.endian_ = !map.endian.empty() ? map.endian : scaling != nullptr ? scaling->endian : "";
        result.from_byte_ = scaling != nullptr ? scaling->from_byte : "x";
        result.to_byte_ = scaling != nullptr ? scaling->to_byte : "x";
        result.width = map.x_size;
        result.height = map.y_size;
        result.start_position = map.start_position;
        result.interval = map.interval;
        result.is_selectable_ = map.type == "Selectable";
    }
    else
    {
        const auto& axis = target == NumericTarget::kXAxis ? map.x_axis : map.y_axis;
        scaling = definition::FindScaling(definition, axis.scaling_name);
        result.address = axis.address;
        result.storage_type = axis.storage_type;
        result.endian_ = axis.endian;
        result.from_byte_ = axis.from_byte;
        result.to_byte_ = axis.to_byte;
        result.width = target == NumericTarget::kXAxis ? map.x_size : 1;
        result.height = target == NumericTarget::kYAxis ? map.y_size : 1;
        result.start_position = axis.start_position;
        result.interval = axis.interval;
        result.is_selectable_ = axis.type == "Selectable";
    }
    result.byte_width = ElementByteWidth(result.storage_type, scaling);
    if (scaling != nullptr)
    {
        result.min_value_ = scaling->minimum;
        result.max_value_ = scaling->maximum;
        result.coarse_increment_ = IncrementValue(scaling->coarse_increment);
        result.fine_increment_ = IncrementValue(scaling->fine_increment);
    }
    return result;
}
} // namespace fastecu::calibration::internal
