#include "src/algorithms/memory/memory_map.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include <gtest/gtest.h>

namespace fastecu::memory
{
namespace
{
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

MemoryBlock FillBlock(std::uint32_t start, std::uint32_t size)
{
    return MemoryBlock{.range = Range(start, size), .backing = FillBacking{}, .writability = Writability::kReadOnly};
}

// The 160 KiB MC68HC16Y5 `_02` ROM file: file 0x00000-0x1FFFF at 0x00000,
// RAM at 0x20000-0x27FFF, file 0x20000-0x27FFF at 0x28000.
std::array<MemoryBlock, 3> PackedMc68Blocks()
{
    return {FileBlock(0x0, 0x20000, 0x0), FillBlock(0x20000, 0x8000), FileBlock(0x28000, 0x8000, 0x20000)};
}

std::optional<MemoryErrorKind> CreateError(std::span<const MemoryBlock> blocks, std::uint32_t file_size)
{
    const auto map = MemoryMap::Create(blocks, ByteCount{file_size});
    return map.has_value() ? std::nullopt : std::optional(map.error().kind);
}

TEST(MemoryMap, AcceptsThePackedMc68Layout)
{
    const auto blocks = PackedMc68Blocks();
    const auto map = MemoryMap::Create(blocks, ByteCount{0x28000});

    ASSERT_TRUE(map.has_value());
    EXPECT_EQ(map->Blocks().size(), 3U);
    EXPECT_EQ(map->FileSize(), ByteCount{0x28000});
}

TEST(MemoryMap, BlockAtFindsTheBlockHoldingAnAddress)
{
    const auto blocks = PackedMc68Blocks();
    const auto map = MemoryMap::Create(blocks, ByteCount{0x28000});
    ASSERT_TRUE(map.has_value());

    EXPECT_EQ(map->BlockAt(FlashAddress{0x0}), &map->Blocks()[0]);
    EXPECT_EQ(map->BlockAt(FlashAddress{0x1FFFF}), &map->Blocks()[0]);
    EXPECT_EQ(map->BlockAt(FlashAddress{0x20000}), &map->Blocks()[1]);
    EXPECT_EQ(map->BlockAt(FlashAddress{0x2FFFF}), &map->Blocks()[2]);
    EXPECT_EQ(map->BlockAt(FlashAddress{0x30000}), nullptr);
}

TEST(MemoryMap, AcceptsAddressesThatNoBlockHolds)
{
    // A 1N83M-style ROM file placed high in the address space: nothing at 0.
    const std::array blocks{FileBlock(0x08F9C000, 0x100, 0x0)};
    const auto map = MemoryMap::Create(blocks, ByteCount{0x100});

    ASSERT_TRUE(map.has_value());
    EXPECT_EQ(map->BlockAt(FlashAddress{0x0}), nullptr);
    EXPECT_EQ(map->BlockAt(FlashAddress{0x08F9C0FF}), &map->Blocks()[0]);
}

TEST(MemoryMap, IdentityHoldsTheWholeFileWritableAtAddressZero)
{
    const auto map = MemoryMap::Identity(ByteCount{0x80000});

    ASSERT_TRUE(map.has_value());
    ASSERT_EQ(map->Blocks().size(), 1U);
    const MemoryBlock& block = map->Blocks()[0];
    EXPECT_EQ(block.range, Range(0x0, 0x80000));
    EXPECT_EQ(block.backing, (std::variant<FileBacking, FillBacking>{FileBacking{.offset = FileOffset{0}}}));
    EXPECT_EQ(block.writability, Writability::kWritable);
}

TEST(MemoryMap, IdentityRejectsAnEmptyFile)
{
    const auto map = MemoryMap::Identity(ByteCount{0});

    ASSERT_FALSE(map.has_value());
    EXPECT_EQ(map.error().kind, MemoryErrorKind::kInvalidLayout);
}

TEST(MemoryMap, RejectsAMapWithNoBlocks)
{
    EXPECT_EQ(CreateError({}, 0x100), MemoryErrorKind::kInvalidLayout);
}

TEST(MemoryMap, RejectsOverlappingBlocks)
{
    const std::array blocks{FileBlock(0x0, 0x100, 0x0), FileBlock(0x80, 0x100, 0x100)};
    EXPECT_EQ(CreateError(blocks, 0x200), MemoryErrorKind::kInvalidLayout);
}

TEST(MemoryMap, RejectsBlocksOutOfAddressOrder)
{
    const std::array blocks{FileBlock(0x100, 0x100, 0x100), FileBlock(0x0, 0x100, 0x0)};
    EXPECT_EQ(CreateError(blocks, 0x200), MemoryErrorKind::kInvalidLayout);
}

TEST(MemoryMap, RejectsFileBytesPlacedTwice)
{
    const std::array blocks{FileBlock(0x0, 0x100, 0x0), FileBlock(0x100, 0x100, 0x0)};
    EXPECT_EQ(CreateError(blocks, 0x100), MemoryErrorKind::kInvalidLayout);
}

TEST(MemoryMap, RejectsFileBytesLeftUnplaced)
{
    const std::array tail_missing{FileBlock(0x0, 0x100, 0x0)};
    EXPECT_EQ(CreateError(tail_missing, 0x200), MemoryErrorKind::kInvalidLayout);

    const std::array middle_missing{FileBlock(0x0, 0x100, 0x0), FileBlock(0x100, 0x100, 0x180)};
    EXPECT_EQ(CreateError(middle_missing, 0x280), MemoryErrorKind::kInvalidLayout);
}

TEST(MemoryMap, RejectsBlocksRunningPastTheFile)
{
    const std::array blocks{FileBlock(0x0, 0x200, 0x0)};
    EXPECT_EQ(CreateError(blocks, 0x100), MemoryErrorKind::kInvalidLayout);
}

TEST(MemoryMap, AcceptsFileRangesPlacedOutOfFileOrder)
{
    const std::array blocks{FileBlock(0x0, 0x10, 0x10), FileBlock(0x10, 0x10, 0x0)};
    EXPECT_EQ(CreateError(blocks, 0x20), std::nullopt);
}

TEST(MemoryMap, RejectsAWritableFillBlock)
{
    // A fill block has no ROM file bytes, so nothing can be written to it.
    const std::array blocks{
        FileBlock(0x0, 0x100, 0x0),
        MemoryBlock{.range = Range(0x100, 0x100), .backing = FillBacking{}, .writability = Writability::kWritable}};
    EXPECT_EQ(CreateError(blocks, 0x100), MemoryErrorKind::kInvalidLayout);
}

TEST(MemoryMap, RejectsAnEmptyRomFile)
{
    const std::array blocks{FillBlock(0x0, 0x100)};
    EXPECT_EQ(CreateError(blocks, 0), MemoryErrorKind::kInvalidLayout);
}
TEST(MemoryMap, DefinitionBaseIsZeroUnlessDeclared)
{
    const auto blocks = PackedMc68Blocks();
    const auto map = MemoryMap::Create(blocks, ByteCount{0x28000});

    ASSERT_TRUE(map.has_value());
    EXPECT_EQ(map->DefinitionBase(), FlashAddress{0});
}

TEST(MemoryMap, KeepsTheDeclaredDefinitionBase)
{
    // 1N83M definitions count addresses from where the ROM file starts.
    const std::array blocks{FileBlock(0x08F9C000, 0x100, 0x0)};
    const auto map = MemoryMap::Create(blocks, ByteCount{0x100}, FlashAddress{0x08F9C000});

    ASSERT_TRUE(map.has_value());
    EXPECT_EQ(map->DefinitionBase(), FlashAddress{0x08F9C000});
}

TEST(MemoryMap, ToFlashAddressAddsTheDefinitionBase)
{
    const std::array blocks{FileBlock(0x08F9C000, 0x40, 0x0)};
    const auto map = MemoryMap::Create(blocks, ByteCount{0x40}, FlashAddress{0x08F9C000});
    ASSERT_TRUE(map.has_value());

    EXPECT_EQ(map->ToFlashAddress(DefinitionAddress{0x10}), FlashAddress{0x08F9C010});
}

TEST(MemoryMap, ToFlashAddressFailsPastTheAddressSpace)
{
    const std::array blocks{FileBlock(0xFFFFFF00, 0x40, 0x0)};
    const auto map = MemoryMap::Create(blocks, ByteCount{0x40}, FlashAddress{0xFFFFFF00});
    ASSERT_TRUE(map.has_value());

    EXPECT_EQ(map->ToFlashAddress(DefinitionAddress{0xFF}), FlashAddress{0xFFFFFFFF});
    EXPECT_EQ(map->ToFlashAddress(DefinitionAddress{0x100}), std::nullopt);
}

TEST(MemoryMap, FileOffsetOfFollowsTheBlocksFileBacking)
{
    const auto blocks = PackedMc68Blocks();
    const auto map = MemoryMap::Create(blocks, ByteCount{0x28000});
    ASSERT_TRUE(map.has_value());

    EXPECT_EQ(map->FileOffsetOf(Range(0x10, 4)), FileOffset{0x10});
    EXPECT_EQ(map->FileOffsetOf(Range(0x28010, 4)), FileOffset{0x20010});
}

TEST(MemoryMap, FileOffsetOfNeedsOneFileBackedBlockToHoldTheWholeRange)
{
    const auto blocks = PackedMc68Blocks();
    const auto map = MemoryMap::Create(blocks, ByteCount{0x28000});
    ASSERT_TRUE(map.has_value());

    EXPECT_EQ(map->FileOffsetOf(Range(0x20010, 4)), std::nullopt); // fill block
    EXPECT_EQ(map->FileOffsetOf(Range(0x1FFFE, 4)), std::nullopt); // into the fill block
    EXPECT_EQ(map->FileOffsetOf(Range(0x30000, 4)), std::nullopt); // no block
}

TEST(MemoryMap, FileOffsetOfDoesNotSpanAdjacentFileBlocks)
{
    const std::array blocks{FileBlock(0x0, 0x10, 0x0), FileBlock(0x10, 0x10, 0x10, Writability::kReadOnly)};
    const auto map = MemoryMap::Create(blocks, ByteCount{0x20});
    ASSERT_TRUE(map.has_value());

    EXPECT_EQ(map->FileOffsetOf(Range(0x0E, 4)), std::nullopt);
}

TEST(MemoryMapSpan, RunsFromTheFirstBlockStartToTheLastBlockEnd)
{
    const std::array blocks{FileBlock(0x100, 0x10, 0x0, Writability::kReadOnly), FileBlock(0x200, 0x20, 0x10)};
    const auto map = MemoryMap::Create(blocks, ByteCount{0x30});
    ASSERT_TRUE(map.has_value());

    EXPECT_EQ(map->Span(), Range(0x100, 0x120));
}

} // namespace
} // namespace fastecu::memory
