#include "src/algorithms/memory/memory_image.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace fastecu::memory
{
namespace
{
std::unexpected<MemoryError> Fail(MemoryErrorKind kind, std::string detail)
{
    return std::unexpected(MemoryError{.kind = kind, .detail = std::move(detail)});
}

// One stretch of a range that lies inside a single block.
struct Chunk
{
    const MemoryBlock *block{nullptr};
    FlashAddress start;
    std::uint32_t size{0};
    std::size_t offset_in_range{0};
};

// Splits `range` at block boundaries, in address order. Fails at the first
// chunk that no block holds (kUnmapped) or that `check` rejects, so the error
// names the first address that fails.
template <typename Check>
std::expected<std::vector<Chunk>, MemoryError> SplitByBlock(const MemoryMap& map, AddressRange<FlashSpace> range,
                                                            Check check)
{
    std::vector<Chunk> chunks;
    FlashAddress cursor = range.Start();
    while (cursor < range.End())
    {
        const MemoryBlock *block = map.BlockAt(cursor);
        if (block == nullptr)
        {
            return Fail(MemoryErrorKind::kUnmapped,
                        std::format("no memory block holds address 0x{:x}", cursor.Value()));
        }
        const FlashAddress chunk_end = std::min(block->range.End(), range.End());
        const Chunk chunk{.block = block,
                          .start = cursor,
                          .size = chunk_end.Value() - cursor.Value(),
                          .offset_in_range = cursor.Value() - range.Start().Value()};
        if (const std::expected<void, MemoryError> checked = check(chunk); !checked.has_value())
        {
            return std::unexpected(checked.error());
        }
        chunks.push_back(chunk);
        cursor = chunk_end;
    }
    return chunks;
}

std::expected<void, MemoryError> AnyBlock(const Chunk& /*chunk*/)
{
    return {};
}

std::expected<void, MemoryError> WritableFileBlock(const Chunk& chunk)
{
    if (std::holds_alternative<FillBacking>(chunk.block->backing))
    {
        return Fail(MemoryErrorKind::kFillBlock, std::format("address 0x{:x} is in a fill block", chunk.start.Value()));
    }
    if (chunk.block->writability != Writability::kWritable)
    {
        return Fail(MemoryErrorKind::kNotWritable,
                    std::format("address 0x{:x} is in a block that is not writable", chunk.start.Value()));
    }
    return {};
}

// Where `address`, inside a file-backed `block`, sits in the ROM file.
std::size_t FileIndex(const FileBacking& file, const MemoryBlock& block, FlashAddress address)
{
    return std::size_t{file.offset.Value()} + (address.Value() - block.range.Start().Value());
}
} // namespace

AddressRange<FlashSpace> MemoryView::Range() const
{
    return range_;
}

bytes::ByteView MemoryView::Data() const
{
    return data_;
}

MemoryView::MemoryView(AddressRange<FlashSpace> range, bytes::Bytes data) : range_(range), data_(std::move(data))
{
}

std::expected<MemoryImage, MemoryError> MemoryImage::Create(MemoryMap map, bytes::Bytes file)
{
    if (file.size() != map.FileSize().Value())
    {
        return Fail(
            MemoryErrorKind::kFileSizeMismatch,
            std::format("the ROM file is {} bytes; its memory map places {}", file.size(), map.FileSize().Value()));
    }
    return MemoryImage(std::move(map), std::move(file));
}

const MemoryMap& MemoryImage::Map() const
{
    return map_;
}

bytes::ByteView MemoryImage::File() const
{
    return file_;
}

std::expected<MemoryView, MemoryError> MemoryImage::Render(AddressRange<FlashSpace> range) const
{
    const auto chunks = SplitByBlock(map_, range, AnyBlock);
    if (!chunks.has_value())
    {
        return std::unexpected(chunks.error());
    }
    bytes::Bytes data(range.Size().Value());
    for (const Chunk& chunk : *chunks)
    {
        const auto out = data.begin() + static_cast<std::ptrdiff_t>(chunk.offset_in_range);
        if (const auto *file = std::get_if<FileBacking>(&chunk.block->backing); file != nullptr)
        {
            const auto from = file_.begin() + static_cast<std::ptrdiff_t>(FileIndex(*file, *chunk.block, chunk.start));
            std::ranges::copy_n(from, static_cast<std::ptrdiff_t>(chunk.size), out);
        }
        else
        {
            std::ranges::fill_n(out, static_cast<std::ptrdiff_t>(chunk.size),
                                std::get<FillBacking>(chunk.block->backing).value);
        }
    }
    return MemoryView(range, std::move(data));
}

std::expected<void, MemoryError> MemoryImage::CheckWrite(FlashAddress start, ByteCount size) const
{
    if (size.Value() == 0)
    {
        return {};
    }
    const auto range = AddressRange<FlashSpace>::Make(start, size);
    if (!range.has_value())
    {
        return Fail(MemoryErrorKind::kUnmapped,
                    std::format("a {}-byte write at 0x{:x} runs past the address space", size.Value(), start.Value()));
    }
    const auto chunks = SplitByBlock(map_, *range, WritableFileBlock);
    if (!chunks.has_value())
    {
        return std::unexpected(chunks.error());
    }
    return {};
}

std::expected<void, MemoryError> MemoryImage::Write(FlashAddress start, bytes::ByteView data)
{
    if (data.empty())
    {
        return {};
    }
    const std::optional<ByteCount> size = ByteCount::FromSize(data.size());
    const auto range = size.has_value() ? AddressRange<FlashSpace>::Make(start, *size) : std::nullopt;
    if (!range.has_value())
    {
        return Fail(MemoryErrorKind::kUnmapped,
                    std::format("a {}-byte write at 0x{:x} runs past the address space", data.size(), start.Value()));
    }
    // Every chunk is checked before any byte changes, so a rejected write changes nothing.
    const auto chunks = SplitByBlock(map_, *range, WritableFileBlock);
    if (!chunks.has_value())
    {
        return std::unexpected(chunks.error());
    }
    for (const Chunk& chunk : *chunks)
    {
        const auto& file = std::get<FileBacking>(chunk.block->backing);
        std::ranges::copy(data.subspan(chunk.offset_in_range, chunk.size),
                          file_.begin() + static_cast<std::ptrdiff_t>(FileIndex(file, *chunk.block, chunk.start)));
    }
    return {};
}

MemoryImage::MemoryImage(MemoryMap map, bytes::Bytes file) : map_(std::move(map)), file_(std::move(file))
{
}
} // namespace fastecu::memory
