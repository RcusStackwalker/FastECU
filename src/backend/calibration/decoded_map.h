#pragma once

#include <string>
#include <variant>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{
using NumericCell = Result<double>;

struct NumericRun
{
    std::vector<NumericCell> cells;
    bool operator==(const NumericRun&) const = default;
};

struct StaticAxis
{
    std::vector<std::string> labels;
    bool operator==(const StaticAxis&) const = default;
};

struct BlobValue
{
    bytes::Bytes data;
    bool operator==(const BlobValue&) const = default;
};

using AxisValue = std::variant<std::monostate, NumericRun, StaticAxis>;

// A transient snapshot. Cell errors retain usable storage layout; an outer
// Result<DecodedMap> error means the map's layout itself is unusable.
struct DecodedMap
{
    std::variant<NumericRun, BlobValue> body;
    AxisValue x_axis;
    AxisValue y_axis;
    bool operator==(const DecodedMap&) const = default;
};
} // namespace fastecu::calibration
