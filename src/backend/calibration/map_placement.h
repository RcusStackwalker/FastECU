#pragma once

#include <cstdint>

#include "src/algorithms/memory/memory_map.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{
// Definition addresses [start, end) holding every run a placement check
// covered; start == end when the map has no addressed run.
struct MapExtent
{
    std::uint64_t start{0};
    std::uint64_t end{0};

    bool operator==(const MapExtent&) const = default;
};

// A structural map failure (ADR 0020) when a run of `map` -- its cells, X axis
// or Y axis -- reaches an address no block of `memory_map` holds, touches a fill
// block, or spans writable and read-only blocks. Definition addresses count from
// memory_map.DefinitionBase(). A run without an address, and a cell run with
// zero or overflowing dimensions, is left to the decoder to reject. On success,
// the extent from the lowest checked run to the highest.
Result<MapExtent> CheckMapPlacement(const definition::RomDefinition& definition, const definition::CalibrationMap& map,
                                    const memory::MemoryMap& memory_map);
} // namespace fastecu::calibration
