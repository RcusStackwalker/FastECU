#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "src/backend/definition/definition_model.h"

namespace fastecu::calibration
{

// One run of consecutive elements: a map's cells, or one axis's points. Built
// from either a CalibrationMap or an AxisDefinition -- the three call sites
// (map cells, X axis, Y axis) differ only in which fields they read.
//
// A non-owning view: `endian` and `from_byte` borrow from the RomDefinition
// (the map's scaling or the resolved axis fields) used to build the run, which
// always outlives the DecodeNumericRun call. Never store one.
struct ElementRun
{
    std::uint64_t address{0};
    std::uint32_t count{0};
    std::uint32_t start_position{1};
    std::uint32_t interval{1};
    std::optional<definition::StorageType> storage_type;
    std::string_view endian;
    std::string_view from_byte{"x"};
    bool is_selectable{false};
};

} // namespace fastecu::calibration
