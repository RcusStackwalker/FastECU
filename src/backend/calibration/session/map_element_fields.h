#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "src/backend/calibration/map_edit.h"
#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/definition/definition_model.h"

namespace fastecu::calibration
{

// Owns the std::strings a MapElementSpec's string_views point at. Copy/move
// are compiler defaults: a short string typically lives inline under
// small-string optimization, so relocating this object dangles a spec()
// already taken from it. The enforced invariant is narrower: never call
// spec() on an object whose lifetime has ended or is about to. spec()'s
// ref-qualification closes the temporary case at compile time; callers bind
// the fields once and never relocate them while a spec is in use.
class MapElementFields
{
  public:
    MapElementSpec Spec() const&;
    MapElementSpec spec() const&& = delete;

  private:
    friend MapElementFields CollectMapElementFields(const CalibrationSession&, std::size_t, NumericTarget);

    MapElementFields() = default;

    std::string endian_;
    std::string to_byte_;
    std::string from_byte_;
    std::string min_value_;
    std::string max_value_;
    std::uint64_t address_{0};
    std::optional<definition::StorageType> storage_type_;
    double coarse_increment_{0.0};
    double fine_increment_{0.0};
    std::uint32_t x_size_{1};
    std::uint32_t y_size_{1};
    std::uint32_t start_position_{1};
    std::uint32_t interval_{1};
};

// Resolves definition and protocol fields for one element run. Body storage
// and endian use map fields before scaling defaults; axes use their resolved
// axis fields. Requires a definition with a map at `map_index`.
MapElementFields CollectMapElementFields(const CalibrationSession& session, std::size_t map_index,
                                         NumericTarget target);

} // namespace fastecu::calibration
