#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "src/backend/calibration/map_edit.h"
#include "src/backend/definitions/ecu_cal_def.h"

namespace fastecu::ui
{

// Owns the std::strings a MapElementSpec's string_views point at. A spec built
// directly from QString::toStdString() temporaries dangles the moment the
// statement ends -- the single easiest way to get this wrong.
class MapElementFields
{
  public:
    calibration::MapElementSpec spec() const;

  private:
    friend MapElementFields collect_map_element_fields(const definitions::EcuCalDefStructure&, int,
                                                       calibration::EditTargetKind);

    std::string endian_;
    std::string to_byte_;
    std::string from_byte_;
    std::string min_value_;
    std::string max_value_;
    std::string flash_method_;
    std::uint64_t address_{0};
    std::optional<definition::StorageType> storage_type_;
    double coarse_increment_{0.0};
    double fine_increment_{0.0};
    std::uint32_t x_size_{1};
    std::uint32_t y_size_{1};
    std::uint64_t rom_file_size_{0};
};

// Plucks one element run's fields out of the Qt-typed model. `kind` selects
// between the map-body lists, the XScale* lists, and the YScale* lists -- the
// field-plucking half of what the three duplicated legacy blocks did.
//
// Takes fastecu::definitions::EcuCalDefStructure rather than
// FileActions::EcuCalDefStructure (the same type, via FileActions's `using
// EcuCalDefStructure = fastecu::definitions::EcuCalDefStructure` alias) so
// this package can depend on the lightweight //src/backend/definitions:ecu_cal_def
// target instead of pulling in the full legacy FileActions god object.
// Callers holding a FileActions::EcuCalDefStructure pass it in unchanged.
MapElementFields collect_map_element_fields(const definitions::EcuCalDefStructure& def, int map_number,
                                            calibration::EditTargetKind kind);

} // namespace fastecu::ui
