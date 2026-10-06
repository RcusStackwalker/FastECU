#include "src/backend/calibration/session/map_element_fields.h"

#include <limits>
#include <string_view>

#include "src/algorithms/expression/checked_expression.h"

namespace fastecu::calibration
{
namespace
{

std::string legacy_text(std::string_view text)
{
    return text.empty() ? " " : std::string(text);
}

double increment_value(std::string_view text)
{
    if (text.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos)
    {
        return 0.0;
    }
    const auto value = expression::parse_finite_number(text);
    // Reject malformed increments in the numeric increment operation without
    // preventing absolute assignments that do not use increment metadata.
    return value.has_value() ? *value : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

MapElementSpec MapElementFields::spec() const&
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

MapElementFields collect_map_element_fields(const CalibrationSession& session, std::size_t map_index,
                                            NumericTarget target)
{
    MapElementFields fields;
    const auto& def = session.definition()->definition;
    const auto& map = def.maps.at(map_index);
    const definition::Scaling *scaling = nullptr;
    if (target == NumericTarget::MapBody)
    {
        scaling = definition::find_scaling(def, map.scaling_name);
        fields.address_ = map.address.value_or(0);
        fields.storage_type_ = map.storage_type ? map.storage_type : scaling ? scaling->storage_type : std::nullopt;
        fields.endian_ = legacy_text(!map.endian.empty() ? map.endian : scaling ? scaling->endian : "");
        fields.from_byte_ = scaling ? legacy_text(scaling->from_byte) : "x";
        fields.to_byte_ = scaling ? legacy_text(scaling->to_byte) : "x";
        fields.start_position_ = map.start_position;
        fields.interval_ = map.interval;
    }
    else
    {
        const auto& axis = target == NumericTarget::XAxis ? map.x_axis : map.y_axis;
        const bool present = !axis.type.empty();
        scaling = present ? definition::find_scaling(def, axis.scaling_name) : nullptr;
        fields.address_ = present ? axis.address.value_or(0) : 0;
        fields.storage_type_ = present ? axis.storage_type : std::nullopt;
        fields.endian_ = present ? legacy_text(axis.endian) : " ";
        fields.from_byte_ = present ? legacy_text(axis.from_byte) : " ";
        fields.to_byte_ = present ? legacy_text(axis.to_byte) : " ";
        fields.start_position_ = present ? axis.start_position : 1;
        fields.interval_ = present ? axis.interval : 1;
    }
    fields.min_value_ = scaling ? legacy_text(scaling->minimum) : " ";
    fields.max_value_ = scaling ? legacy_text(scaling->maximum) : " ";
    fields.coarse_increment_ = scaling ? increment_value(scaling->coarse_increment) : 0.0;
    fields.fine_increment_ = scaling ? increment_value(scaling->fine_increment) : 0.0;
    fields.x_size_ = map.x_size;
    fields.y_size_ = map.y_size;
    fields.flash_method_ = session.protocol().flash_method;
    fields.rom_file_size_ = session.protocol().unpadded_size;
    return fields;
}

} // namespace fastecu::calibration
