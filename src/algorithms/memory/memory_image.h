#pragma once

#include <expected>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_error.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/algorithms/protocol/bytes.h"

namespace fastecu::memory
{
// A contiguous copy of one address range, rendered from a MemoryImage for
// consumers that index bytes directly. It does not follow later writes to the
// image and is never stored back into it.
class MemoryView
{
  public:
    [[nodiscard]] AddressRange<FlashSpace> Range() const;
    [[nodiscard]] bytes::ByteView Data() const;

  private:
    friend class MemoryImage;
    MemoryView(AddressRange<FlashSpace> range, bytes::Bytes data);

    AddressRange<FlashSpace> range_;
    bytes::Bytes data_;
};

// A ROM file's bytes placed at ECU addresses by a memory map. The ROM file
// bytes are the only stored copy; reads and writes by address go through the map.
class MemoryImage
{
  public:
    // kFileSizeMismatch unless `file` is exactly the size `map` places.
    static std::expected<MemoryImage, MemoryError> Create(MemoryMap map, bytes::Bytes file);

    [[nodiscard]] const MemoryMap& Map() const;
    // The ROM file bytes in file layout, as saving writes them.
    [[nodiscard]] bytes::ByteView File() const;

    // Every address in `range` must belong to a block (kUnmapped otherwise);
    // fill blocks read as their fill byte.
    [[nodiscard]] std::expected<MemoryView, MemoryError> Render(AddressRange<FlashSpace> range) const;

    // Writes `data` from `start` into the ROM file bytes. Every address written
    // must be in a writable, file-backed block (kUnmapped, kFillBlock or
    // kNotWritable otherwise, for the first address that is not); on any error
    // nothing is written. Writing no bytes succeeds.
    std::expected<void, MemoryError> Write(FlashAddress start, bytes::ByteView data);

  private:
    MemoryImage(MemoryMap map, bytes::Bytes file);

    MemoryMap map_;
    bytes::Bytes file_;
};
} // namespace fastecu::memory
