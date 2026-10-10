#pragma once

#include <expected>
#include <span>
#include <variant>
#include <vector>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_error.h"
#include "src/algorithms/protocol/bytes.h"

namespace fastecu::memory
{
// The block holds ROM file bytes, the first of them at `offset`.
struct FileBacking
{
    FileOffset offset;

    constexpr bool operator==(const FileBacking&) const = default;
};

// The block has no ROM file bytes; every address in it reads as `value`.
struct FillBacking
{
    bytes::Byte value{0xFF};

    constexpr bool operator==(const FillBacking&) const = default;
};

// Whether the protocol can write a block to the ECU by any route.
enum class Writability
{
    kReadOnly,
    kWritable,
};

// One contiguous range of ECU addresses in a memory map.
struct MemoryBlock
{
    AddressRange<FlashSpace> range;
    std::variant<FileBacking, FillBacking> backing;
    Writability writability{Writability::kReadOnly};
};

// The ordered memory blocks that place one ROM file layout at ECU addresses.
class MemoryMap
{
  public:
    // Blocks must be at least one, sorted by address and disjoint, and no fill
    // block may be writable. The ROM file must be non-empty, and together the
    // file-backed blocks must place every byte of it exactly once. Addresses
    // between blocks are allowed and belong to no block. `definition_base` is
    // the ECU address that address 0 in a definition file stands for (ADR 0020);
    // it need not lie in a block.
    static std::expected<MemoryMap, MemoryError> Create(std::span<const MemoryBlock> blocks, ByteCount file_size,
                                                        FlashAddress definition_base = FlashAddress{0});

    // One writable block holding the whole ROM file at address 0.
    static std::expected<MemoryMap, MemoryError> Identity(ByteCount file_size);

    [[nodiscard]] std::span<const MemoryBlock> Blocks() const;
    [[nodiscard]] ByteCount FileSize() const;
    [[nodiscard]] FlashAddress DefinitionBase() const;

    // The block holding `address`, or nullptr when no block does.
    [[nodiscard]] const MemoryBlock *BlockAt(FlashAddress address) const;

  private:
    MemoryMap(std::vector<MemoryBlock> blocks, ByteCount file_size, FlashAddress definition_base);

    std::vector<MemoryBlock> blocks_;
    ByteCount file_size_;
    FlashAddress definition_base_;
};
} // namespace fastecu::memory
