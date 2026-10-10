#include "src/backend/calibration/map_placement.h"

#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "src/backend/calibration/calibration_service.h"

namespace fastecu::calibration
{
namespace
{
// Why the definition addresses [start, end) have no valid storage in
// `memory_map`, or nullopt when they do.
std::optional<std::string_view> ProblemWith(std::uint64_t start, std::uint64_t end, const memory::MemoryMap& memory_map)
{
    constexpr std::string_view kOutside = "outside the ROM's memory map";
    constexpr std::uint64_t kAddressSpaceEnd = std::uint64_t{1} << 32;
    const std::uint64_t base = memory_map.DefinitionBase().Value();
    const std::optional<memory::FlashAddress> first =
        start > std::numeric_limits<std::uint32_t>::max()
            ? std::nullopt
            : memory_map.ToFlashAddress(memory::DefinitionAddress{static_cast<std::uint32_t>(start)});
    if (!first.has_value() || end > kAddressSpaceEnd - base)
    {
        return kOutside;
    }
    std::optional<memory::Writability> writability;
    for (std::uint64_t cursor = first->Value(); cursor < base + end;)
    {
        const memory::MemoryBlock *block = memory_map.BlockAt(memory::FlashAddress{static_cast<std::uint32_t>(cursor)});
        if (block == nullptr)
        {
            return kOutside;
        }
        if (std::holds_alternative<memory::FillBacking>(block->backing))
        {
            return "in a fill block, which holds no ROM file bytes";
        }
        if (writability.has_value() && *writability != block->writability)
        {
            return "across writable and read-only memory";
        }
        writability = block->writability;
        cursor = block->range.End().Value();
    }
    return std::nullopt;
}

Status CheckRun(std::string_view map_name, std::string_view subject, std::optional<memory::DefinitionAddress> address,
                std::uint64_t count, std::uint32_t start_position, std::uint32_t interval, std::uint32_t width,
                const memory::MemoryMap& memory_map)
{
    if (!address.has_value() || count == 0 || count > std::numeric_limits<std::uint32_t>::max())
    {
        return {};
    }
    const std::uint64_t end =
        ElementRunEnd(address->Value(), start_position, interval, width, static_cast<std::uint32_t>(count));
    if (const std::optional<std::string_view> problem = ProblemWith(address->Value(), end, memory_map);
        problem.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("map '{}' {} {}", map_name, subject, *problem));
    }
    return {};
}
} // namespace

Status CheckMapPlacement(const definition::RomDefinition& definition, const definition::CalibrationMap& map,
                         const memory::MemoryMap& memory_map)
{
    const definition::Scaling *scaling = definition::FindScaling(definition, map.scaling_name);
    const auto storage = map.storage_type.has_value() ? map.storage_type
                         : scaling != nullptr         ? scaling->storage_type
                                                      : std::nullopt;
    const std::uint32_t width = ElementByteSize(storage, scaling);
    // A blob is read whole at its address, whatever its stride fields say.
    const Status cells = storage == definition::StorageType::kBloblist
                             ? CheckRun(map.name, "cells lie", map.address, 1, 1, 1, width, memory_map)
                             : CheckRun(map.name, "cells lie", map.address, std::uint64_t{map.x_size} * map.y_size,
                                        map.start_position, map.interval, width, memory_map);
    if (!cells.has_value())
    {
        return cells;
    }
    for (const auto& [axis, subject] : {std::pair{&map.x_axis, "X axis lies"}, std::pair{&map.y_axis, "Y axis lies"}})
    {
        const std::uint32_t axis_width =
            ElementByteSize(axis->storage_type, definition::FindScaling(definition, axis->scaling_name));
        if (const Status placed = CheckRun(map.name, subject, axis->address, axis->size, axis->start_position,
                                           axis->interval, axis_width, memory_map);
            !placed.has_value())
        {
            return placed;
        }
    }
    return {};
}
} // namespace fastecu::calibration
