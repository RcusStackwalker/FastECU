#include "src/algorithms/memory/memory_image.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::memory
{
namespace
{
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

AddressRange<FlashSpace> Range(std::uint32_t start, std::uint32_t size)
{
    return AddressRange<FlashSpace>::Make(FlashAddress{start}, ByteCount{size}).value();
}

MemoryBlock FileBlock(std::uint32_t start, std::uint32_t size, std::uint32_t file_offset,
                      Writability writability = Writability::kWritable)
{
    return MemoryBlock{.range = Range(start, size),
                       .backing = FileBacking{.offset = FileOffset{file_offset}},
                       .writability = writability};
}

MemoryBlock FillBlock(std::uint32_t start, std::uint32_t size, bytes::Byte value = 0xFF)
{
    return MemoryBlock{
        .range = Range(start, size), .backing = FillBacking{.value = value}, .writability = Writability::kReadOnly};
}

MemoryImage ImageOf(std::span<const MemoryBlock> blocks, bytes::Bytes file)
{
    auto map = MemoryMap::Create(blocks, ByteCount::FromSize(file.size()).value());
    return MemoryImage::Create(std::move(map).value(), std::move(file)).value();
}

std::optional<MemoryErrorKind> WriteError(MemoryImage& image, std::uint32_t start, bytes::ByteView data)
{
    const auto written = image.Write(FlashAddress{start}, data);
    return written.has_value() ? std::nullopt : std::optional(written.error().kind);
}

// The 160 KiB MC68HC16Y5 `_02` ROM file with marker bytes on both sides of the RAM gap.
MemoryImage PackedMc68Image()
{
    bytes::Bytes file(0x28000, 0x00);
    file[0x1FFFF] = 0xA1; // last byte before the gap, at 0x1FFFF
    file[0x20000] = 0xB2; // first byte after the gap, at 0x28000
    file[0x27FFF] = 0xC3; // last byte, at 0x2FFFF
    const std::array blocks{FileBlock(0x0, 0x20000, 0x0), FillBlock(0x20000, 0x8000),
                            FileBlock(0x28000, 0x8000, 0x20000)};
    return ImageOf(blocks, std::move(file));
}

TEST(MemoryImage, CreateRejectsAFileOfAnotherSize)
{
    const auto map = MemoryMap::Identity(ByteCount{0x100});
    ASSERT_TRUE(map.has_value());

    const auto image = MemoryImage::Create(*map, bytes::Bytes(0x200, 0x00));

    ASSERT_FALSE(image.has_value());
    EXPECT_EQ(image.error().kind, MemoryErrorKind::kFileSizeMismatch);
}

TEST(MemoryImage, FileIsTheRomFileAsLoaded)
{
    const MemoryImage image = PackedMc68Image();

    EXPECT_EQ(image.File().size(), 0x28000U);
    EXPECT_EQ(image.File()[0x20000], 0xB2);
}

TEST(MemoryImage, RenderPlacesFileBytesAtTheirEcuAddresses)
{
    const MemoryImage image = PackedMc68Image();

    const auto before_gap = image.Render(Range(0x1FFFF, 1));
    const auto after_gap = image.Render(Range(0x28000, 1));
    const auto last = image.Render(Range(0x2FFFF, 1));

    ASSERT_TRUE(before_gap.has_value());
    ASSERT_TRUE(after_gap.has_value());
    ASSERT_TRUE(last.has_value());
    EXPECT_THAT(before_gap->Data(), ElementsAre(0xA1));
    EXPECT_THAT(after_gap->Data(), ElementsAre(0xB2));
    EXPECT_THAT(last->Data(), ElementsAre(0xC3));
    EXPECT_EQ(after_gap->Range(), Range(0x28000, 1));
}

TEST(MemoryImage, RenderAcrossAFillBlockReadsItsFillByte)
{
    const MemoryImage image = PackedMc68Image();

    const auto view = image.Render(Range(0x1FFFF, 0x8002));

    ASSERT_TRUE(view.has_value());
    const bytes::ByteView data = view->Data();
    ASSERT_EQ(data.size(), 0x8002U);
    EXPECT_EQ(data.front(), 0xA1);
    EXPECT_EQ(data.back(), 0xB2);
    const bytes::Bytes gap(data.begin() + 1, data.end() - 1);
    EXPECT_THAT(gap, Each(0xFF));
}

TEST(MemoryImage, RenderUsesEachFillBlocksOwnByte)
{
    const std::array blocks{FillBlock(0x0, 0x4, 0x00), FileBlock(0x4, 0x4, 0x0)};
    const MemoryImage image = ImageOf(blocks, bytes::Bytes{1, 2, 3, 4});

    const auto view = image.Render(Range(0x0, 0x8));

    ASSERT_TRUE(view.has_value());
    EXPECT_THAT(view->Data(), ElementsAre(0x00, 0x00, 0x00, 0x00, 1, 2, 3, 4));
}

TEST(MemoryImage, RenderRejectsAddressesNoBlockHolds)
{
    const MemoryImage image = PackedMc68Image();

    const auto view = image.Render(Range(0x2FFFF, 2));

    ASSERT_FALSE(view.has_value());
    EXPECT_EQ(view.error().kind, MemoryErrorKind::kUnmapped);
}

TEST(MemoryImage, ARenderedViewDoesNotFollowLaterWrites)
{
    MemoryImage image = PackedMc68Image();
    const auto view = image.Render(Range(0x28000, 1));
    ASSERT_TRUE(view.has_value());

    ASSERT_EQ(WriteError(image, 0x28000, bytes::Bytes{0x55}), std::nullopt);

    EXPECT_THAT(view->Data(), ElementsAre(0xB2));
}

TEST(MemoryImage, WriteGoesThroughToTheRomFileBytes)
{
    MemoryImage image = PackedMc68Image();

    ASSERT_EQ(WriteError(image, 0x28000, bytes::Bytes{0x5A}), std::nullopt);

    EXPECT_EQ(image.File()[0x20000], 0x5A);
}

TEST(MemoryImage, WriteAcrossBlocksFollowsEachBlocksFileRange)
{
    // Two adjacent blocks whose file ranges are swapped.
    const std::array blocks{FileBlock(0x0, 0x10, 0x10), FileBlock(0x10, 0x10, 0x0)};
    MemoryImage image = ImageOf(blocks, bytes::Bytes(0x20, 0x00));

    ASSERT_EQ(WriteError(image, 0x0E, bytes::Bytes{1, 2, 3, 4}), std::nullopt);

    EXPECT_EQ(image.File()[0x1E], 1);
    EXPECT_EQ(image.File()[0x1F], 2);
    EXPECT_EQ(image.File()[0x00], 3);
    EXPECT_EQ(image.File()[0x01], 4);
}

TEST(MemoryImage, WriteIntoAFillBlockChangesNothing)
{
    MemoryImage image = PackedMc68Image();
    const bytes::Bytes before(image.File().begin(), image.File().end());

    EXPECT_EQ(WriteError(image, 0x1FFFF, bytes::Bytes{1, 2}), MemoryErrorKind::kFillBlock);

    EXPECT_THAT(image.File(), ElementsAreArray(before));
}

TEST(MemoryImage, WriteIntoAReadOnlyBlockChangesNothing)
{
    // A bootloader prefix the protocol never writes, then writable userspace.
    const std::array blocks{FileBlock(0x0, 0x8000, 0x0, Writability::kReadOnly), FileBlock(0x8000, 0x8000, 0x8000)};
    MemoryImage image = ImageOf(blocks, bytes::Bytes(0x10000, 0x00));

    EXPECT_EQ(WriteError(image, 0x7FFF, bytes::Bytes{1, 2}), MemoryErrorKind::kNotWritable);
    EXPECT_EQ(image.File()[0x8000], 0x00);

    EXPECT_EQ(WriteError(image, 0x8000, bytes::Bytes{1}), std::nullopt);
    EXPECT_EQ(image.File()[0x8000], 1);
}

TEST(MemoryImage, WritePastTheMapChangesNothing)
{
    MemoryImage image = PackedMc68Image();

    EXPECT_EQ(WriteError(image, 0x2FFFF, bytes::Bytes{1, 2}), MemoryErrorKind::kUnmapped);

    EXPECT_EQ(image.File()[0x27FFF], 0xC3);
}

TEST(MemoryImage, WriteReportsTheFirstAddressThatCannotBeWritten)
{
    // Writable userspace, then a block the protocol never writes, then no block.
    const std::array blocks{FileBlock(0x0, 0x8000, 0x0), FileBlock(0x8000, 0x8000, 0x8000, Writability::kReadOnly)};
    MemoryImage image = ImageOf(blocks, bytes::Bytes(0x10000, 0x00));
    const bytes::Bytes before(image.File().begin(), image.File().end());

    EXPECT_EQ(WriteError(image, 0x7FFF, bytes::Bytes(0x8002, 0x11)), MemoryErrorKind::kNotWritable);

    EXPECT_THAT(image.File(), ElementsAreArray(before));
}

TEST(MemoryImage, WritingNoBytesSucceeds)
{
    MemoryImage image = PackedMc68Image();

    EXPECT_EQ(WriteError(image, 0x20000, bytes::ByteView{}), std::nullopt);
}
TEST(MemoryImage, CheckWriteReportsWhatWriteWouldWithoutWriting)
{
    const MemoryImage image = PackedMc68Image();
    const bytes::Bytes before(image.File().begin(), image.File().end());
    const auto check = [&image](std::uint32_t start, std::uint32_t size) -> std::optional<MemoryErrorKind>
    {
        const auto checked = image.CheckWrite(FlashAddress{start}, ByteCount{size});
        return checked.has_value() ? std::nullopt : std::optional(checked.error().kind);
    };

    EXPECT_EQ(check(0x28000, 0x10), std::nullopt);
    EXPECT_EQ(check(0x1FFFF, 2), MemoryErrorKind::kFillBlock);
    EXPECT_EQ(check(0x2FFFF, 2), MemoryErrorKind::kUnmapped);
    EXPECT_EQ(check(0xFFFFFFFF, 1), MemoryErrorKind::kUnmapped);
    EXPECT_EQ(check(0x20000, 0), std::nullopt);
    EXPECT_THAT(image.File(), ElementsAreArray(before));
}

TEST(MemoryImage, CheckWriteRejectsReadOnlyBlocks)
{
    const std::array blocks{FileBlock(0x0, 0x8000, 0x0, Writability::kReadOnly), FileBlock(0x8000, 0x8000, 0x8000)};
    const MemoryImage image = ImageOf(blocks, bytes::Bytes(0x10000, 0x00));

    const auto checked = image.CheckWrite(FlashAddress{0x7FFF}, ByteCount{2});

    ASSERT_FALSE(checked.has_value());
    EXPECT_EQ(checked.error().kind, MemoryErrorKind::kNotWritable);
}

} // namespace
} // namespace fastecu::memory
