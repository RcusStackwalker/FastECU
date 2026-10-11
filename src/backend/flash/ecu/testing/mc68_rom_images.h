#pragma once

#include <utility>

#include "src/algorithms/memory/memory_image.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/config/builtin_catalog.h"
#include "src/backend/config/catalog.h"

namespace fastecu::flash::testing
{
// MC68HC16Y5 `_02` ROM files placed exactly as the built-in catalog places
// them (sub_ecu_denso_mc68hc16y5_02's memory maps): flash at 0x00000-0x1FFFF
// and 0x28000-0x2FFFF around RAM at 0x20000-0x27FFF.
inline memory::MemoryImage CatalogMc68Image(bytes::Bytes file)
{
    return config::PlaceRomFile(config::BuiltinCatalog().FindProtocol("sub_ecu_denso_mc68hc16y5_02"), std::move(file))
        .value();
}

// A 160 KiB file: the two flash ranges packed, the RAM range a 0xFF fill block.
inline memory::MemoryImage PackedMc68Image(bytes::Bytes packed)
{
    return CatalogMc68Image(std::move(packed));
}

// A 192 KiB file holding `packed`'s flash bytes at their ECU addresses and
// 0xFF for the read-only RAM range, as a BDM read or a community file does.
inline memory::MemoryImage FullMc68Image(bytes::ByteView packed)
{
    bytes::Bytes full(packed.begin(), packed.end());
    full.insert(full.begin() + 0x20000, 0x8000, bytes::Byte{0xFF});
    return CatalogMc68Image(std::move(full));
}
} // namespace fastecu::flash::testing
