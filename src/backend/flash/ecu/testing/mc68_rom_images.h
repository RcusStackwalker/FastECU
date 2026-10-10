#pragma once

#include <array>
#include <cstdint>
#include <utility>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_image.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/algorithms/protocol/bytes.h"

namespace fastecu::flash::testing
{
// MC68HC16Y5 `_02` ROM files placed as the built-in catalog places them
// (builtin_catalog.cpp kMc68PackedBlocks/kMc68FullBlocks): flash at
// 0x00000-0x1FFFF and 0x28000-0x2FFFF around RAM at 0x20000-0x27FFF.

inline memory::AddressRange<memory::FlashSpace> Mc68Range(std::uint32_t start, std::uint32_t size)
{
    return memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{start}, memory::ByteCount{size}).value();
}

inline memory::MemoryBlock Mc68FileBlock(std::uint32_t start, std::uint32_t size, std::uint32_t offset,
                                         memory::Writability writability)
{
    return {.range = Mc68Range(start, size),
            .backing = memory::FileBacking{.offset = memory::FileOffset{offset}},
            .writability = writability};
}

// A 160 KiB file: the two flash ranges packed, the RAM range a 0xFF fill block.
inline memory::MemoryImage PackedMc68Image(bytes::Bytes packed)
{
    const std::array blocks{
        Mc68FileBlock(0x00000, 0x20000, 0x00000, memory::Writability::kWritable),
        memory::MemoryBlock{.range = Mc68Range(0x20000, 0x8000),
                            .backing = memory::FillBacking{.value = 0xFF},
                            .writability = memory::Writability::kReadOnly},
        Mc68FileBlock(0x28000, 0x08000, 0x20000, memory::Writability::kWritable),
    };
    return memory::MemoryImage::Create(memory::MemoryMap::Create(blocks, memory::ByteCount{0x28000}).value(),
                                       std::move(packed))
        .value();
}

// A 192 KiB file holding `packed`'s flash bytes at their ECU addresses and
// 0xFF for the read-only RAM range, as a BDM read or a community file does.
inline memory::MemoryImage FullMc68Image(bytes::ByteView packed)
{
    bytes::Bytes full(packed.begin(), packed.end());
    full.insert(full.begin() + 0x20000, 0x8000, bytes::Byte{0xFF});
    const std::array blocks{
        Mc68FileBlock(0x00000, 0x20000, 0x00000, memory::Writability::kWritable),
        Mc68FileBlock(0x20000, 0x08000, 0x20000, memory::Writability::kReadOnly),
        Mc68FileBlock(0x28000, 0x08000, 0x28000, memory::Writability::kWritable),
    };
    return memory::MemoryImage::Create(memory::MemoryMap::Create(blocks, memory::ByteCount{0x30000}).value(),
                                       std::move(full))
        .value();
}

// `file` at ECU address 0 and writable throughout.
inline memory::MemoryImage IdentityImage(bytes::Bytes file)
{
    const auto size = memory::ByteCount::FromSize(file.size()).value();
    return memory::MemoryImage::Create(memory::MemoryMap::Identity(size).value(), std::move(file)).value();
}
} // namespace fastecu::flash::testing
