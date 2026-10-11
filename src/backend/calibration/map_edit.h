#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{
enum class EditTargetKind
{
    kMapBody,
    kXAxis,
    kYAxis,
    kRejected,
};

// Widget coordinates reserve row/column zero for axis headers where present.
struct SelectionRange
{
    int first_row{0};
    int first_col{0};
    int last_row{0};
    int last_col{0};
};

struct MapDimensions
{
    std::uint32_t x_size{0};
    std::uint32_t y_size{0};
};

// range is translated into element coordinates. x_size is the selected run's
// width, which is one for Y axes and differs from the map's own geometry.
struct EditTarget
{
    EditTargetKind kind{EditTargetKind::kMapBody};
    SelectionRange range;
    std::uint32_t x_size{0};
};

// A numeric edit's semantic target, independent of any table layout. Element
// coordinates inside a target follow that run's own geometry.
enum class NumericTarget
{
    kMapBody,
    kXAxis,
    kYAxis,
};

EditTarget ResolveEditTarget(const SelectionRange& selection, MapDimensions dims, std::string_view x_scale_type);

// Borrowed metadata for one element run. Keep its owning definition-field
// snapshot alive for the whole call; do not store this view. Limit text comes
// from external definition metadata and is parsed once per numeric edit.
struct MapElementSpec
{
    std::uint64_t address{0};
    std::optional<definition::StorageType> storage_type;
    std::string_view endian;
    std::string_view to_byte{"x"};
    std::string_view from_byte{"x"};
    std::string_view min_value{" "};
    std::string_view max_value{" "};
    double coarse_increment{0.0};
    double fine_increment{0.0};
    // Map geometry, not the selected run's width. Use EditTarget::x_size for
    // indexing a Y-axis edit rather than this x_size.
    std::uint32_t x_size{1};
    std::uint32_t y_size{1};
    // One-based start and element interval; callers validate nonzero values.
    std::uint32_t start_position{1};
    std::uint32_t interval{1};
};

// Checked layout arithmetic returns a sentinel on overflow.
std::uint64_t ElementByteAddress(const MapElementSpec& spec, std::uint32_t index);

// Float reads return IEEE-754 bits, not a scaled numeric value. All integer
// widths respect declared byte order; float storage remains big-endian.
Result<std::int64_t> ReadRawElement(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t index);

// Packs already-encoded raw values. Numeric encoding checks range before
// calling this primitive. Float raw values carry the stored bit pattern.
Result<bytes::Bytes> WriteRawElement(const MapElementSpec& spec, std::int64_t raw);

enum class IncrementStep
{
    kFineUp,
    kFineDown,
    kCoarseUp,
    kCoarseDown,
};

enum class InterpolationMode
{
    kHorizontal,
    kVertical,
    kBidirectional,
};
} // namespace fastecu::calibration
