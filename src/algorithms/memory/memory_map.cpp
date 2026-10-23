#include "src/algorithms/memory/memory_map.h"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace fastecu::memory
{
namespace
{
std::unexpected<MemoryError> InvalidLayout(std::string detail)
{
    return std::unexpected(MemoryError{.kind = MemoryErrorKind::kInvalidLayout, .detail = std::move(detail)});
}

// [start, end) for start < end, both addresses in the 32-bit space.
AddressRange<FlashSpace> Between(FlashAddress start, FlashAddress end)
{
    return *AddressRange<FlashSpace>::Make(start, *end.DistanceFrom(start));
}

// Sorted by file offset, the file-backed blocks' file ranges must tile
// [0, file_size) with no gap and no overlap.
std::expected<void, MemoryError> CheckFilePlacement(std::span<const MemoryBlock> blocks, ByteCount file_size)
{
    std::vector<std::pair<FileOffset, ByteCount>> file_ranges;
    for (const MemoryBlock& block : blocks)
    {
        if (const auto *file = std::get_if<FileBacking>(&block.backing); file != nullptr)
        {
            file_ranges.emplace_back(file->offset, block.range.Size());
        }
    }
    std::ranges::sort(file_ranges);

    FileOffset next{0};
    for (const auto& [offset, size] : file_ranges)
    {
        if (offset < next)
        {
            return InvalidLayout(std::format("ROM file byte 0x{:x} is placed by two blocks", offset.Value()));
        }
        if (next < offset)
        {
            return InvalidLayout(
                std::format("ROM file bytes 0x{:x}-0x{:x} are not placed", next.Value(), offset.Value() - 1));
        }
        const std::optional<FileOffset> end = offset.Advance(size);
        if (!end.has_value())
        {
            return InvalidLayout("a file-backed block runs past the 32-bit file range");
        }
        next = *end;
    }

    const FileOffset file_end{file_size.Value()};
    if (next < file_end)
    {
        return InvalidLayout(std::format("ROM file bytes from 0x{:x} are not placed", next.Value()));
    }
    if (file_end < next)
    {
        return InvalidLayout(std::format("file-backed blocks run past the {}-byte ROM file", file_size.Value()));
    }
    return {};
}
} // namespace

std::expected<MemoryMap, MemoryError> MemoryMap::Create(std::span<const MemoryBlock> blocks, ByteCount file_size,
                                                        FlashAddress definition_base)
{
    if (blocks.empty())
    {
        return InvalidLayout("a memory map needs at least one block");
    }
    if (file_size.Value() == 0)
    {
        return InvalidLayout("a memory map needs a non-empty ROM file");
    }
    const auto writable_fill = std::ranges::find_if(
        blocks, [](const MemoryBlock& block)
        { return std::holds_alternative<FillBacking>(block.backing) && block.writability == Writability::kWritable; });
    if (writable_fill != blocks.end())
    {
        return InvalidLayout(
            std::format("the fill block at 0x{:x} is marked writable", writable_fill->range.Start().Value()));
    }
    const auto disorder = std::ranges::adjacent_find(blocks, [](const MemoryBlock& before, const MemoryBlock& after)
                                                     { return after.range.Start() < before.range.End(); });
    if (disorder != blocks.end())
    {
        return InvalidLayout(std::format("the block at 0x{:x} overlaps or precedes the block before it",
                                         std::next(disorder)->range.Start().Value()));
    }
    if (const auto placement = CheckFilePlacement(blocks, file_size); !placement.has_value())
    {
        return std::unexpected(placement.error());
    }
    return MemoryMap(std::vector<MemoryBlock>(blocks.begin(), blocks.end()), file_size, definition_base);
}

std::expected<MemoryMap, MemoryError> MemoryMap::Identity(ByteCount file_size)
{
    const auto range = AddressRange<FlashSpace>::Make(FlashAddress{0}, file_size);
    if (!range.has_value())
    {
        return InvalidLayout("an identity memory map needs a non-empty ROM file");
    }
    const std::array blocks{MemoryBlock{
        .range = *range, .backing = FileBacking{.offset = FileOffset{0}}, .writability = Writability::kWritable}};
    return Create(blocks, file_size);
}

std::span<const MemoryBlock> MemoryMap::Blocks() const
{
    return blocks_;
}

AddressRange<FlashSpace> MemoryMap::Span() const
{
    const FlashAddress start = blocks_.front().range.Start();
    // Create guarantees sorted, non-empty blocks whose ends are representable.
    return *AddressRange<FlashSpace>::Make(start, *blocks_.back().range.End().DistanceFrom(start));
}

ByteCount MemoryMap::FileSize() const
{
    const ByteCount size = file_size_;
    return size;
}

FlashAddress MemoryMap::DefinitionBase() const
{
    return definition_base_;
}

const MemoryBlock *MemoryMap::BlockAt(FlashAddress address) const
{
    // Blocks are sorted and disjoint, so only the last block starting at or
    // before `address` can hold it.
    const auto after =
        std::ranges::upper_bound(blocks_, address, {}, [](const MemoryBlock& block) { return block.range.Start(); });
    if (after == blocks_.begin())
    {
        return nullptr;
    }
    const MemoryBlock& candidate = *std::prev(after);
    return candidate.range.Contains(address) ? &candidate : nullptr;
}

std::optional<FlashAddress> MemoryMap::ToFlashAddress(DefinitionAddress address) const
{
    return definition_base_.Advance(ByteCount{address.Value()});
}

std::vector<BlockSlice> MemoryMap::Slices(AddressRange<FlashSpace> range) const
{
    std::vector<BlockSlice> slices;
    // Blocks are sorted and disjoint: start at the first one ending after the
    // range starts, and walk forward.
    auto block = std::ranges::upper_bound(blocks_, range.Start(), {},
                                          [](const MemoryBlock& candidate) { return candidate.range.End(); });
    FlashAddress cursor = range.Start();
    while (cursor < range.End())
    {
        if (block == blocks_.end() || cursor < block->range.Start())
        {
            const FlashAddress gap_end =
                block == blocks_.end() ? range.End() : std::min(block->range.Start(), range.End());
            slices.push_back(BlockSlice{.block = nullptr, .range = Between(cursor, gap_end)});
            cursor = gap_end;
            continue;
        }
        const FlashAddress end = std::min(block->range.End(), range.End());
        slices.push_back(BlockSlice{.block = std::to_address(block), .range = Between(cursor, end)});
        cursor = end;
        ++block;
    }
    return slices;
}

std::optional<FileOffset> MemoryMap::FileOffsetOf(AddressRange<FlashSpace> range) const
{
    const std::vector<BlockSlice> slices = Slices(range);
    return slices.size() == 1 ? slices.front().FileStart() : std::nullopt;
}

std::optional<FileOffset> BlockSlice::FileStart() const
{
    if (block == nullptr)
    {
        return std::nullopt;
    }
    const auto *file = std::get_if<FileBacking>(&block->backing);
    if (file == nullptr)
    {
        return std::nullopt;
    }
    // The slice lies inside its block, so the distance exists and the offset
    // fits in the file.
    return file->offset.Advance(*range.Start().DistanceFrom(block->range.Start()));
}

MemoryMap::MemoryMap(std::vector<MemoryBlock> blocks, ByteCount file_size, FlashAddress definition_base)
    : blocks_(std::move(blocks)), file_size_(file_size), definition_base_(definition_base)
{
}
} // namespace fastecu::memory
