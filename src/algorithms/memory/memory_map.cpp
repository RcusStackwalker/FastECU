#include "src/algorithms/memory/memory_map.h"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
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

std::expected<MemoryMap, MemoryError> MemoryMap::Create(std::span<const MemoryBlock> blocks, ByteCount file_size)
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
    return MemoryMap(std::vector<MemoryBlock>(blocks.begin(), blocks.end()), file_size);
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

ByteCount MemoryMap::FileSize() const
{
    return file_size_;
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

MemoryMap::MemoryMap(std::vector<MemoryBlock> blocks, ByteCount file_size)
    : blocks_(std::move(blocks)), file_size_(file_size)
{
}
} // namespace fastecu::memory
