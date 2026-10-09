#pragma once

#include "src/backend/calibration/decoded_map.h"
#include "src/backend/calibration/map_edit.h"

namespace fastecu::calibration
{
struct CellWrite
{
    std::uint32_t index;
    std::uint64_t byte_address;
    bytes::Bytes bytes;
    bool operator==(const CellWrite&) const = default;
};

using NumericEditPatch = std::vector<CellWrite>;

enum class NoChangeReason
{
    kNone,
    kUnchanged,
    kBelowStorageResolution,
    kDefinitionLimit,
    kMultipleCauses,
};

struct NumericEditResult
{
    NumericEditPatch writes;
    NoChangeReason no_change{NoChangeReason::kNone};
};

Result<NumericEditResult> calculate_increment(bytes::ByteView rom, const MapElementSpec& spec, std::uint32_t run_width,
                                              std::span<const NumericCell> cells, const SelectionRange& range,
                                              IncrementStep step);
Result<NumericEditResult> calculate_assignment(bytes::ByteView rom, const MapElementSpec& spec, std::uint32_t run_width,
                                               std::span<const NumericCell> cells, const SelectionRange& range,
                                               std::string_view expression);
Result<NumericEditResult> calculate_interpolation(bytes::ByteView rom, const MapElementSpec& spec,
                                                  std::uint32_t run_width, std::span<const NumericCell> cells,
                                                  const SelectionRange& range, InterpolationMode mode);
Result<NumericEditResult> calculate_paste(bytes::ByteView rom, const MapElementSpec& spec, std::uint32_t run_width,
                                          std::uint32_t run_height, const SelectionRange& range,
                                          std::span<const std::vector<double>> values);
} // namespace fastecu::calibration
