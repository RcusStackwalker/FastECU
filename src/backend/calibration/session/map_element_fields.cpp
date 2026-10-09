#include "src/backend/calibration/session/map_element_fields.h"

#include <limits>
#include <string_view>

#include "src/algorithms/expression/expression.h"

namespace fastecu::calibration
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
    // Reject malformed increments in the numeric increment operation without
    // preventing absolute assignments that do not use increment metadata.
    return value.has_value() ? *value : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

MapElementSpec MapElementFields::Spec() const&
{
    MapElementSpec spec;
    spec.address = address_;
    spec.storage_type = storage_type_;
    spec.endian = endian_;
    spec.to_byte = to_byte_;
    spec.from_byte = from_byte_;
    spec.min_value = min_value_;
    spec.max_value = max_value_;
    spec.coarse_increment = coarse_increment_;
    spec.fine_increment = fine_increment_;
    spec.x_size = x_size_;
    spec.y_size = y_size_;
    spec.start_position = start_position_;
    spec.interval = interval_;
    spec.flash_method = flash_method_;
    spec.rom_file_size = rom_file_size_;
    return spec;
}

MapElementFields CollectMapElementFields(const CalibrationSession& session, std::size_t map_index, NumericTarget target)
{
    MapElementFields fields;
    const auto& def = session.Definition()->definition;
    const auto& map = def.maps.at(map_index);
    const definition::Scaling *scaling = nullptr;
    if (target == NumericTarget::kMapBody)
    {
        scaling = definition::FindScaling(def, map.scaling_name);
        fields.address_ = map.address.value_or(0);
        fields.storage_type_ = map.storage_type ? map.storage_type : scaling ? scaling->storage_type : std::nullopt;
        fields.endian_ = !map.endian.empty() ? map.endian : scaling ? scaling->endian : "";
        fields.from_byte_ = scaling ? scaling->from_byte : "x";
        fields.to_byte_ = scaling ? scaling->to_byte : "x";
        fields.start_position_ = map.start_position;
        fields.interval_ = map.interval;
    }
    else
    {
        const auto& axis = target == NumericTarget::kXAxis ? map.x_axis : map.y_axis;
        const bool present = !axis.type.empty();
        scaling = present ? definition::FindScaling(def, axis.scaling_name) : nullptr;
        fields.address_ = present ? axis.address.value_or(0) : 0;
        fields.storage_type_ = present ? axis.storage_type : std::nullopt;
        fields.endian_ = present ? axis.endian : "";
        fields.from_byte_ = present ? axis.from_byte : "";
        fields.to_byte_ = present ? axis.to_byte : "";
        fields.start_position_ = present ? axis.start_position : 1;
        fields.interval_ = present ? axis.interval : 1;
    }
    fields.min_value_ = scaling ? scaling->minimum : "";
    fields.max_value_ = scaling ? scaling->maximum : "";
    fields.coarse_increment_ = scaling ? IncrementValue(scaling->coarse_increment) : 0.0;
    fields.fine_increment_ = scaling ? IncrementValue(scaling->fine_increment) : 0.0;
    fields.x_size_ = map.x_size;
    fields.y_size_ = map.y_size;
    fields.flash_method_ = session.Protocol().flash_method;
    fields.rom_file_size_ = session.Protocol().unpadded_size;
    return fields;
}

} // namespace fastecu::calibration
