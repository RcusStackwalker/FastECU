#include "src/backend/calibration/map_placement.h"

#include <array>
#include <cstdint>
#include <limits>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration
{
namespace
{
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::HasSubstr;

constexpr auto kReadOnly = memory::Writability::kReadOnly;
constexpr auto kWritable = memory::Writability::kWritable;

memory::AddressRange<memory::FlashSpace> Range(std::uint32_t start, std::uint32_t size)
{
    return memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{start}, memory::ByteCount{size}).value();
}

memory::MemoryBlock File(std::uint32_t start, std::uint32_t size, std::uint32_t offset, memory::Writability writability)
{
    return {.range = Range(start, size),
            .backing = memory::FileBacking{.offset = memory::FileOffset{offset}},
            .writability = writability};
}

memory::MemoryBlock Fill(std::uint32_t start, std::uint32_t size)
{
    return {.range = Range(start, size), .backing = memory::FillBacking{}, .writability = kReadOnly};
}

// From `base`: 0x00-0x0F read-only, 0x10-0x1F writable, 0x20-0x2F fill,
// 0x30-0x3F writable, nothing past 0x40. Definitions count from `base`.
memory::MemoryMap Layout(std::uint32_t base = 0)
{
    const std::array blocks{File(base + 0x00, 0x10, 0x00, kReadOnly), File(base + 0x10, 0x10, 0x10, kWritable),
                            Fill(base + 0x20, 0x10), File(base + 0x30, 0x10, 0x20, kWritable)};
    return memory::MemoryMap::Create(blocks, memory::ByteCount{0x30}, memory::FlashAddress{base}).value();
}

definition::RomDefinition OneMap(std::uint64_t address, std::uint32_t cells)
{
    definition::RomDefinition definition;
    definition::CalibrationMap map;
    map.name = "Fuel";
    map.address = address;
    map.x_size = cells;
    map.y_size = 1;
    map.storage_type = definition::StorageType::kUint8;
    definition.maps.push_back(map);
    return definition;
}

Status Check(const definition::RomDefinition& definition, const memory::MemoryMap& layout)
{
    return CheckMapPlacement(definition, definition.maps[0], layout);
}

TEST(CheckMapPlacement, AcceptsAMapInsideOneWritableBlock)
{
    EXPECT_THAT(Check(OneMap(0x10, 0x10), Layout()), IsOk());
}

// Q13: a map in read-only memory still decodes; only its edits are rejected.
TEST(CheckMapPlacement, AcceptsAMapInsideOneReadOnlyBlock)
{
    EXPECT_THAT(Check(OneMap(0x00, 0x10), Layout()), IsOk());
}

TEST(CheckMapPlacement, RejectsAMapThatTouchesAFillBlock)
{
    EXPECT_THAT(Check(OneMap(0x1F, 2), Layout()),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("map 'Fuel' cells lie in a fill block")));
}

TEST(CheckMapPlacement, RejectsAMapThatSpansWritableAndReadOnlyBlocks)
{
    EXPECT_THAT(Check(OneMap(0x0F, 2), Layout()),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("across writable and read-only memory")));
}

TEST(CheckMapPlacement, RejectsAMapThatLeavesTheMemoryMap)
{
    EXPECT_THAT(Check(OneMap(0x3F, 2), Layout()),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("outside the ROM's memory map")));
    EXPECT_THAT(Check(OneMap(std::numeric_limits<std::uint32_t>::max(), 2), Layout()),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("outside the ROM's memory map")));
}

TEST(CheckMapPlacement, ChecksEachAxisToo)
{
    auto definition = OneMap(0x10, 2);
    definition.maps[0].x_axis.address = 0x2F;
    definition.maps[0].x_axis.size = 2;
    definition.maps[0].x_axis.storage_type = definition::StorageType::kUint8;

    EXPECT_THAT(Check(definition, Layout()),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("map 'Fuel' X axis lies in a fill block")));
}

TEST(CheckMapPlacement, CountsDefinitionAddressesFromTheDefinitionBase)
{
    EXPECT_THAT(Check(OneMap(0x10, 0x10), Layout(0x08F9C000)), IsOk());
    EXPECT_THAT(Check(OneMap(0x20, 1), Layout(0x08F9C000)),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("fill block")));
}

TEST(CheckMapPlacement, LeavesAMapWithoutAnAddressToTheDecoder)
{
    auto definition = OneMap(0, 1);
    definition.maps[0].address.reset();

    EXPECT_THAT(Check(definition, Layout()), IsOk());
}
} // namespace
} // namespace fastecu::calibration
