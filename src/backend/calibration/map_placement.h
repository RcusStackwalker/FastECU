#pragma once

#include "src/algorithms/memory/memory_map.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{
// A structural map failure (ADR 0020) when a run of `map` -- its cells, X axis
// or Y axis -- reaches an address no block of `memory_map` holds, touches a fill
// block, or spans writable and read-only blocks. Definition addresses count from
// memory_map.DefinitionBase(). A run without an address, and a cell run with
// zero or overflowing dimensions, is left to the decoder to reject.
Status CheckMapPlacement(const definition::RomDefinition& definition, const definition::CalibrationMap& map,
                         const memory::MemoryMap& memory_map);
} // namespace fastecu::calibration
