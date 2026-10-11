#include "src/algorithms/memory/memory_image.h"

#include <algorithm>
#include <cstddef>
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

// kUnmapped for a gap; Render reads every block, fill blocks included.
std::expected<void, MemoryError> HeldByABlock(const BlockSlice& slice)
{
    if (slice.block == nullptr)
    {
        return Fail(MemoryErrorKind::kUnmapped,
                    std::format("no memory block holds address 0x{:x}", slice.range.Start().Value()));
    }
    return {};
}

std::expected<void, MemoryError> WritableFileBlock(const BlockSlice& slice)
{
    if (auto held = HeldByABlock(slice); !held.has_value())
    {
        return held;
    }
    if (std::holds_alternative<FillBacking>(slice.block->backing))
    {
        return Fail(MemoryErrorKind::kFillBlock,
                    std::format("address 0x{:x} is in a fill block", slice.range.Start().Value()));
    }
    if (slice.block->writability != Writability::kWritable)
    {
        return Fail(MemoryErrorKind::kNotWritable,
                    std::format("address 0x{:x} is in a block that is not writable", slice.range.Start().Value()));
    }
    return {};
}

// `range`'s slices, or the error `check` gives for the first slice it rejects,
// so the error names the first address that fails.
template <typename Check>
std::expected<std::vector<BlockSlice>, MemoryError> CheckedSlices(const MemoryMap& map, AddressRange<FlashSpace> range,
                                                                  Check check)
{
    std::vector<BlockSlice> slices = map.Slices(range);
    for (const BlockSlice& slice : slices)
    {
        if (auto checked = check(slice); !checked.has_value())
        {
            return std::unexpected(std::move(checked).error());
        }
    }
    return slices;
}

// Where `slice` starts within `range`.
std::ptrdiff_t OffsetIn(AddressRange<FlashSpace> range, const BlockSlice& slice)
{
    return static_cast<std::ptrdiff_t>(slice.range.Start().Value() - range.Start().Value());
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
    const auto slices = CheckedSlices(map_, range, HeldByABlock);
    if (!slices.has_value())
    {
        return std::unexpected(slices.error());
    }
    bytes::Bytes data(range.Size().Value());
    for (const BlockSlice& slice : *slices)
    {
        const auto out = data.begin() + OffsetIn(range, slice);
        const auto size = static_cast<std::ptrdiff_t>(slice.range.Size().Value());
        if (const std::optional<FileOffset> file_start = slice.FileStart(); file_start.has_value())
        {
            std::ranges::copy_n(file_.begin() + static_cast<std::ptrdiff_t>(file_start->Value()), size, out);
        }
        else
        {
            std::ranges::fill_n(out, size, std::get<FillBacking>(slice.block->backing).value);
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
    const auto slices = CheckedSlices(map_, *range, WritableFileBlock);
    if (!slices.has_value())
    {
        return std::unexpected(slices.error());
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
    // Every slice is checked before any byte changes, so a rejected write changes nothing.
    const auto slices = CheckedSlices(map_, *range, WritableFileBlock);
    if (!slices.has_value())
    {
        return std::unexpected(slices.error());
    }
    for (const BlockSlice& slice : *slices)
    {
        // A writable file-backed block always has a file start.
        const FileOffset file_start = *slice.FileStart();
        std::ranges::copy(data.subspan(static_cast<std::size_t>(OffsetIn(*range, slice)), slice.range.Size().Value()),
                          file_.begin() + static_cast<std::ptrdiff_t>(file_start.Value()));
    }
    return {};
}

MemoryImage::MemoryImage(MemoryMap map, bytes::Bytes file) : map_(std::move(map)), file_(std::move(file))
{
}
} // namespace fastecu::memory
