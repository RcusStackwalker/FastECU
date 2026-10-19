#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "src/backend/calibration/map_edit.h"

namespace fastecu::calibration::internal
{
// An operation-local owning snapshot. Build borrowed views only after the last
// move, and keep this owner alive until their consumers finish.
struct ResolvedElement
{
    std::optional<memory::DefinitionAddress> address;
    std::optional<definition::StorageType> storage_type;
    std::string endian;
    std::string from_byte{"x"};
    std::string to_byte{"x"};
    std::string min_value;
    std::string max_value;
    double coarse_increment{0.0};
    double fine_increment{0.0};
    MapDimensions dimensions;
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t start_position{1};
    std::uint32_t interval{1};
    std::uint32_t byte_width{1};
    std::uint64_t count{0};
    bool is_selectable{false};
    // Owned copy of a body target's scaling selections; empty for axes.
    std::vector<definition::Selection> selections;

    // A bloblist body with at least one named selection to choose from.
    bool IsBlobSelection() const;
    MapElementSpec EditSpec() const&;
    MapElementSpec EditSpec() const&& = delete;
};

ResolvedElement ResolveMapElement(const definition::RomDefinition& definition, const definition::CalibrationMap& map,
                                  NumericTarget target);
std::uint32_t ElementByteWidth(std::optional<definition::StorageType> storage_type, const definition::Scaling *scaling);
} // namespace fastecu::calibration::internal
