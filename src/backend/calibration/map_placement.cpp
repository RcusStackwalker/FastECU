#include "src/backend/calibration/map_placement.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <variant>

#include "src/backend/calibration/calibration_service.h"

namespace fastecu::calibration
{
namespace
{
// Why the definition addresses [start, end) have no valid storage in
// `memory_map`, or nullopt when they do. An empty run touches nothing.
std::optional<std::string_view> ProblemWith(memory::DefinitionAddress start, std::uint64_t end,
                                            const memory::MemoryMap& memory_map)
{
    constexpr std::string_view kOutside = "outside the ROM's memory map";
    const std::optional<memory::FlashAddress> first = memory_map.ToFlashAddress(start);
    if (!first.has_value())
    {
        return kOutside;
    }
    if (end <= start.Value())
    {
        return std::nullopt;
    }
    const std::uint64_t size = end - start.Value();
    const auto range = size > std::numeric_limits<std::uint32_t>::max()
                           ? std::nullopt
                           : memory::AddressRange<memory::FlashSpace>::Make(
                                 *first, memory::ByteCount{static_cast<std::uint32_t>(size)});
    if (!range.has_value())
    {
        return kOutside;
    }
    std::optional<memory::Writability> writability;
    for (const memory::BlockSlice& slice : memory_map.Slices(*range))
    {
        if (slice.block == nullptr)
        {
            return kOutside;
        }
        if (std::holds_alternative<memory::FillBacking>(slice.block->backing))
        {
            return "in a fill block, which holds no ROM file bytes";
        }
        if (writability.has_value() && *writability != slice.block->writability)
        {
            return "across writable and read-only memory";
        }
        writability = slice.block->writability;
    }
    return std::nullopt;
}

// The run's extent once its placement is checked; nullopt for a run the check
// leaves to the decoder.
Result<std::optional<MapExtent>> CheckRun(std::string_view map_name, std::string_view subject,
                                          std::optional<memory::DefinitionAddress> address, std::uint64_t count,
                                          std::uint32_t start_position, std::uint32_t interval, std::uint32_t width,
                                          const memory::MemoryMap& memory_map)
{
    if (!address.has_value() || count == 0 || count > std::numeric_limits<std::uint32_t>::max())
    {
        return std::nullopt;
    }
    const std::uint64_t end =
        ElementRunEnd(address->Value(), start_position, interval, width, static_cast<std::uint32_t>(count));
    if (const std::optional<std::string_view> problem = ProblemWith(*address, end, memory_map); problem.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("map '{}' {} {}", map_name, subject, *problem));
    }
    return MapExtent{.start = address->Value(), .end = end};
}

// `extent` widened to cover `run`.
void Cover(std::optional<MapExtent>& extent, const std::optional<MapExtent>& run)
{
    if (!run.has_value())
    {
        return;
    }
    extent = extent.has_value()
                 ? MapExtent{.start = std::min(extent->start, run->start), .end = std::max(extent->end, run->end)}
                 : run;
}
} // namespace

Result<MapExtent> CheckMapPlacement(const definition::RomDefinition& definition, const definition::CalibrationMap& map,
                                    const memory::MemoryMap& memory_map)
{
    const definition::Scaling *scaling = definition::FindScaling(definition, map.scaling_name);
    const auto storage = map.storage_type.has_value() ? map.storage_type
                         : scaling != nullptr         ? scaling->storage_type
                                                      : std::nullopt;
    const std::uint32_t width = ElementByteSize(storage, scaling);
    // A blob is read whole at its address, whatever its stride fields say.
    const auto cells = storage == definition::StorageType::kBloblist
                           ? CheckRun(map.name, "cells lie", map.address, 1, 1, 1, width, memory_map)
                           : CheckRun(map.name, "cells lie", map.address, std::uint64_t{map.x_size} * map.y_size,
                                      map.start_position, map.interval, width, memory_map);
    if (!cells.has_value())
    {
        return std::unexpected(cells.error());
    }
    std::optional<MapExtent> extent = *cells;
    for (const auto& [axis, axis_extent, subject] :
         {std::tuple{&map.x_axis, map.x_size, "X axis lies"}, std::tuple{&map.y_axis, map.y_size, "Y axis lies"}})
    {
        const std::uint32_t axis_width =
            ElementByteSize(axis->storage_type, definition::FindScaling(definition, axis->scaling_name));
        // The decoder reads an axis as long as the map's own dimension.
        const auto placed = CheckRun(map.name, subject, axis->address, axis_extent, axis->start_position,
                                     axis->interval, axis_width, memory_map);
        if (!placed.has_value())
        {
            return std::unexpected(placed.error());
        }
        Cover(extent, *placed);
    }
    return extent.value_or(MapExtent{});
}
} // namespace fastecu::calibration
