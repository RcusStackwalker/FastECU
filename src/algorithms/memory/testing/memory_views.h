#pragma once

#include <array>
#include <span>
#include <utility>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_image.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/algorithms/protocol/bytes.h"

namespace fastecu::memory::testing
{
// `file` as one file-backed block at `start`. Aborts on an empty file.
inline MemoryImage ImageAt(FlashAddress start, bytes::Bytes file, Writability writability = Writability::kWritable)
{
    const ByteCount size = ByteCount::FromSize(file.size()).value();
    const std::array blocks{MemoryBlock{.range = AddressRange<FlashSpace>::Make(start, size).value(),
                                        .backing = FileBacking{},
                                        .writability = writability}};
    return MemoryImage::Create(MemoryMap::Create(blocks, size).value(), std::move(file)).value();
}

// `file` at ECU address 0 under the identity map. Aborts on an empty file.
inline MemoryImage IdentityImage(bytes::Bytes file)
{
    const ByteCount size = ByteCount::FromSize(file.size()).value();
    return MemoryImage::Create(MemoryMap::Identity(size).value(), std::move(file)).value();
}

// `file` placed by `blocks`, with definitions counting from `definition_base`.
// Aborts on a layout MemoryMap::Create rejects.
inline MemoryImage PlacedImage(std::span<const MemoryBlock> blocks, bytes::Bytes file,
                               FlashAddress definition_base = FlashAddress{0})
{
    const ByteCount size = ByteCount::FromSize(file.size()).value();
    return MemoryImage::Create(MemoryMap::Create(blocks, size, definition_base).value(), std::move(file)).value();
}

// A view of `data` placed at `start`. Aborts on empty data.
inline MemoryView ViewOf(bytes::ByteView data, FlashAddress start = FlashAddress{0})
{
    const MemoryImage image = ImageAt(start, bytes::Bytes(data.begin(), data.end()));
    return image.Render(image.Map().Span()).value();
}
} // namespace fastecu::memory::testing
