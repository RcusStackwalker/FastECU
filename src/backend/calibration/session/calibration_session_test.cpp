#include "src/backend/calibration/session/calibration_session.h"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/memory/memory_map.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration
{
namespace
{

using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

definition::RomDefinition FuelDefinition()
{
    definition::RomDefinition rom{.format = definition::DefinitionFormat::kEcuFlash};
    rom.scalings.push_back(definition::Scaling{.name = "Raw", .from_byte = "x"});
    definition::CalibrationMap map;
    map.name = "Fuel";
    map.type = "2D";
    map.address = 2;
    map.x_size = 3;
    map.storage_type = definition::StorageType::kUint8;
    map.endian = "big";
    map.scaling_name = "Raw";
    rom.maps.push_back(map);
    return rom;
}

SessionContents ContentsWithDefinition()
{
    return SessionContents{
        .source = {.display_name = "a.bin", .path = "/cal/a.bin", .origin = RomOrigin::kFile},
        .rom = {0, 0, 5, 6, 7, 0},
        .definition = ResolvedDefinition{.format = definition::DefinitionFormat::kEcuFlash,
                                         .id = "TEST",
                                         .definition = FuelDefinition()},
        .protocol = {.flash_method = "proto_a"},
    };
}

TEST(CalibrationSessionTest, ExposesWhatItWasBuiltFrom)
{
    const CalibrationSession session(SessionId{7}, ContentsWithDefinition());

    EXPECT_EQ(session.Id(), SessionId{7});
    EXPECT_EQ(session.Source().display_name, "a.bin");
    EXPECT_EQ(session.Source().origin, RomOrigin::kFile);
    EXPECT_EQ(session.Rom().size(), 6U);
    ASSERT_NE(session.Definition(), nullptr);
    EXPECT_EQ(session.Definition()->id, "TEST");
    EXPECT_EQ(session.Protocol().flash_method, "proto_a");
    EXPECT_FALSE(session.Dirty());
}

TEST(CalibrationSessionTest, DecodesAMapFromTheCurrentBytes)
{
    const CalibrationSession session(SessionId{1}, ContentsWithDefinition());

    const auto values = session.DecodeMap(0);

    ASSERT_THAT(values, IsOk());
    EXPECT_THAT(std::get<NumericRun>(values->body).cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(5), fastecu::testing::IsOkAnd(6),
                                       fastecu::testing::IsOkAnd(7)));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(values->x_axis));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(values->y_axis));
}

TEST(CalibrationSessionTest, DecodeRejectsAnOutOfRangeIndex)
{
    const CalibrationSession session(SessionId{1}, ContentsWithDefinition());

    EXPECT_THAT(session.DecodeMap(1), IsErr(ErrorKind::kInvalidConfig));
}

TEST(CalibrationSessionTest, DecodeWithoutADefinitionFails)
{
    SessionContents contents = ContentsWithDefinition();
    contents.definition.reset();
    const CalibrationSession session(SessionId{1}, std::move(contents));

    EXPECT_EQ(session.Definition(), nullptr);
    EXPECT_THAT(session.DecodeMap(0), IsErr(ErrorKind::kInvalidConfig));
}

TEST(CalibrationSessionTest, WrittenBytesAreWhatTheNextDecodeSees)
{
    CalibrationSession session(SessionId{1}, ContentsWithDefinition());
    const std::vector<std::uint8_t> patch{9, 8};

    ASSERT_THAT(session.WriteBytes(3, patch), IsOk());

    EXPECT_TRUE(session.Dirty());
    const auto decoded = session.DecodeMap(0);
    ASSERT_THAT(decoded, IsOk());
    EXPECT_THAT(std::get<NumericRun>(decoded->body).cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(5), fastecu::testing::IsOkAnd(9),
                                       fastecu::testing::IsOkAnd(8)));
}

TEST(CalibrationSessionTest, WriteReachingTheLastByteSucceeds)
{
    CalibrationSession session(SessionId{1}, ContentsWithDefinition());
    const std::vector<std::uint8_t> patch{0xAA};

    ASSERT_THAT(session.WriteBytes(5, patch), IsOk());
    EXPECT_EQ(session.Rom()[5], 0xAA);
}

TEST(CalibrationSessionTest, WritePastTheEndChangesNothing)
{
    CalibrationSession session(SessionId{1}, ContentsWithDefinition());
    const std::vector<std::uint8_t> before(session.Rom().begin(), session.Rom().end());
    const std::vector<std::uint8_t> patch{1, 2};

    EXPECT_THAT(session.WriteBytes(5, patch), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(session.WriteBytes(std::numeric_limits<std::uint64_t>::max(), patch), IsErr(ErrorKind::kInvalidConfig));

    EXPECT_EQ(std::vector<std::uint8_t>(session.Rom().begin(), session.Rom().end()), before);
    EXPECT_FALSE(session.Dirty());
}

TEST(CalibrationSessionTest, ProtocolInfoCanBeReplaced)
{
    CalibrationSession session(SessionId{1}, ContentsWithDefinition());

    session.SetProtocol(RomProtocolInfo{.flash_method = "proto_b", .kernel_path = "/k/b.bin"});

    EXPECT_EQ(session.Protocol().flash_method, "proto_b");
    EXPECT_EQ(session.Protocol().kernel_path, "/k/b.bin");
    EXPECT_FALSE(session.Dirty());
}

memory::MemoryBlock FileBlock(std::uint32_t start, std::uint32_t size, std::uint32_t offset,
                              memory::Writability writability = memory::Writability::kWritable)
{
    return {.range =
                memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{start}, memory::ByteCount{size})
                    .value(),
            .backing = memory::FileBacking{.offset = memory::FileOffset{offset}},
            .writability = writability};
}

memory::MemoryBlock FillBlock(std::uint32_t start, std::uint32_t size)
{
    return {.range =
                memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{start}, memory::ByteCount{size})
                    .value(),
            .backing = memory::FillBacking{},
            .writability = memory::Writability::kReadOnly};
}

// An MC68HC16Y5-style packed 4-byte ROM file: file 0-1 at 0, two fill bytes at
// 2, file 2-3 at 4.
CalibrationSession PackedSession(std::optional<ResolvedDefinition> definition = std::nullopt)
{
    const std::array blocks{FileBlock(0, 2, 0), FillBlock(2, 2), FileBlock(4, 2, 2)};
    return CalibrationSession(
        SessionId{1}, SessionContents{.rom = {1, 2, 3, 4},
                                      .memory_map = memory::MemoryMap::Create(blocks, memory::ByteCount{4}).value(),
                                      .definition = std::move(definition)});
}

TEST(CalibrationSessionMemory, RomPlacesFileBytesAtTheirDefinitionAddresses)
{
    const CalibrationSession session = PackedSession();

    EXPECT_THAT(session.Rom(), ElementsAre(1, 2, 0xFF, 0xFF, 3, 4));
    EXPECT_THAT(session.File(), ElementsAre(1, 2, 3, 4));
}

TEST(CalibrationSessionMemory, AWriteLandsInTheRomFileAtItsMappedOffset)
{
    CalibrationSession session = PackedSession();

    ASSERT_THAT(session.WriteBytes(4, bytes::Bytes{9}), IsOk());

    EXPECT_THAT(session.File(), ElementsAre(1, 2, 9, 4));
    EXPECT_THAT(session.Rom(), ElementsAre(1, 2, 0xFF, 0xFF, 9, 4));
    EXPECT_TRUE(session.Dirty());
}

TEST(CalibrationSessionMemory, AWriteIntoAFillBlockChangesNothing)
{
    CalibrationSession session = PackedSession();

    EXPECT_THAT(session.WriteBytes(1, bytes::Bytes{7, 7}),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("fill block")));

    EXPECT_THAT(session.File(), ElementsAre(1, 2, 3, 4));
    EXPECT_THAT(session.Rom(), ElementsAre(1, 2, 0xFF, 0xFF, 3, 4));
    EXPECT_FALSE(session.Dirty());
}

TEST(CalibrationSessionMemory, AWriteIntoAReadOnlyBlockChangesNothing)
{
    const std::array blocks{FileBlock(0, 2, 0, memory::Writability::kReadOnly), FileBlock(2, 2, 2)};
    CalibrationSession session(
        SessionId{1}, SessionContents{.rom = {1, 2, 3, 4},
                                      .memory_map = memory::MemoryMap::Create(blocks, memory::ByteCount{4}).value()});

    EXPECT_THAT(session.CheckWrite(2, 2), IsOk());
    EXPECT_THAT(session.WriteBytes(1, bytes::Bytes{5, 5}),
                IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("not writable")));

    EXPECT_THAT(session.File(), ElementsAre(1, 2, 3, 4));
    EXPECT_FALSE(session.Dirty());
}

// 1N83M: the ROM file sits at 0x08F9C000 and definitions count from there.
TEST(CalibrationSessionMemory, DefinitionAddressesCountFromTheDefinitionBase)
{
    const std::array blocks{FileBlock(0x08F9C000, 4, 0)};
    CalibrationSession session(
        SessionId{1},
        SessionContents{
            .rom = {1, 2, 3, 4},
            .memory_map =
                memory::MemoryMap::Create(blocks, memory::ByteCount{4}, memory::FlashAddress{0x08F9C000}).value()});

    EXPECT_THAT(session.Rom(), ElementsAre(1, 2, 3, 4));
    ASSERT_THAT(session.WriteBytes(1, bytes::Bytes{9}), IsOk());
    EXPECT_THAT(session.File(), ElementsAre(1, 9, 3, 4));
}

TEST(CalibrationSessionMemory, ContentsWithoutAMemoryMapUseTheIdentityMap)
{
    const CalibrationSession session(SessionId{1}, SessionContents{.rom = {1, 2, 3}});

    EXPECT_THAT(session.Rom(), ElementsAre(1, 2, 3));
    EXPECT_THAT(session.File(), ElementsAre(1, 2, 3));
}

TEST(CalibrationSessionMemory, DecodingAMapInAFillBlockIsAStructuralFailure)
{
    definition::RomDefinition definition = FuelDefinition(); // three cells from address 2
    const CalibrationSession session = PackedSession(ResolvedDefinition{.definition = std::move(definition)});

    EXPECT_THAT(session.DecodeMap(0), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("fill block")));
}

} // namespace
} // namespace fastecu::calibration
