#include "src/backend/calibration/session/calibration_session.h"

#include <cstdint>
#include <limits>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration
{
namespace
{

using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

definition::RomDefinition fuel_definition()
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

SessionContents contents_with_definition()
{
    return SessionContents{
        .source = {.display_name = "a.bin", .path = "/cal/a.bin", .origin = RomOrigin::kFile},
        .rom = {0, 0, 5, 6, 7, 0},
        .definition = ResolvedDefinition{.format = definition::DefinitionFormat::kEcuFlash,
                                         .id = "TEST",
                                         .definition = fuel_definition()},
        .protocol = {.flash_method = "proto_a"},
    };
}

TEST(CalibrationSessionTest, ExposesWhatItWasBuiltFrom)
{
    const CalibrationSession session(SessionId{7}, contents_with_definition());

    EXPECT_EQ(session.id(), SessionId{7});
    EXPECT_EQ(session.source().display_name, "a.bin");
    EXPECT_EQ(session.source().origin, RomOrigin::kFile);
    EXPECT_EQ(session.rom().size(), 6U);
    ASSERT_NE(session.definition(), nullptr);
    EXPECT_EQ(session.definition()->id, "TEST");
    EXPECT_EQ(session.protocol().flash_method, "proto_a");
    EXPECT_FALSE(session.dirty());
}

TEST(CalibrationSessionTest, DecodesAMapFromTheCurrentBytes)
{
    const CalibrationSession session(SessionId{1}, contents_with_definition());

    const auto values = session.decode_map(0);

    ASSERT_THAT(values, IsOk());
    EXPECT_THAT(std::get<NumericRun>(values->body).cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(5), fastecu::testing::IsOkAnd(6),
                                       fastecu::testing::IsOkAnd(7)));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(values->x_axis));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(values->y_axis));
}

TEST(CalibrationSessionTest, DecodeRejectsAnOutOfRangeIndex)
{
    const CalibrationSession session(SessionId{1}, contents_with_definition());

    EXPECT_THAT(session.decode_map(1), IsErr(ErrorKind::kInvalidConfig));
}

TEST(CalibrationSessionTest, DecodeWithoutADefinitionFails)
{
    SessionContents contents = contents_with_definition();
    contents.definition.reset();
    const CalibrationSession session(SessionId{1}, std::move(contents));

    EXPECT_EQ(session.definition(), nullptr);
    EXPECT_THAT(session.decode_map(0), IsErr(ErrorKind::kInvalidConfig));
}

TEST(CalibrationSessionTest, WrittenBytesAreWhatTheNextDecodeSees)
{
    CalibrationSession session(SessionId{1}, contents_with_definition());
    const std::vector<std::uint8_t> patch{9, 8};

    ASSERT_THAT(session.write_bytes(3, patch), IsOk());

    EXPECT_TRUE(session.dirty());
    const auto decoded = session.decode_map(0);
    ASSERT_THAT(decoded, IsOk());
    EXPECT_THAT(std::get<NumericRun>(decoded->body).cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(5), fastecu::testing::IsOkAnd(9),
                                       fastecu::testing::IsOkAnd(8)));
}

TEST(CalibrationSessionTest, WriteReachingTheLastByteSucceeds)
{
    CalibrationSession session(SessionId{1}, contents_with_definition());
    const std::vector<std::uint8_t> patch{0xAA};

    ASSERT_THAT(session.write_bytes(5, patch), IsOk());
    EXPECT_EQ(session.rom()[5], 0xAA);
}

TEST(CalibrationSessionTest, WritePastTheEndChangesNothing)
{
    CalibrationSession session(SessionId{1}, contents_with_definition());
    const std::vector<std::uint8_t> before(session.rom().begin(), session.rom().end());
    const std::vector<std::uint8_t> patch{1, 2};

    EXPECT_THAT(session.write_bytes(5, patch), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(session.write_bytes(std::numeric_limits<std::uint64_t>::max(), patch),
                IsErr(ErrorKind::kInvalidConfig));

    EXPECT_EQ(std::vector<std::uint8_t>(session.rom().begin(), session.rom().end()), before);
    EXPECT_FALSE(session.dirty());
}

TEST(CalibrationSessionTest, ProtocolInfoCanBeReplaced)
{
    CalibrationSession session(SessionId{1}, contents_with_definition());

    session.set_protocol(RomProtocolInfo{.flash_method = "proto_b", .kernel_path = "/k/b.bin"});

    EXPECT_EQ(session.protocol().flash_method, "proto_b");
    EXPECT_EQ(session.protocol().kernel_path, "/k/b.bin");
    EXPECT_FALSE(session.dirty());
}

} // namespace
} // namespace fastecu::calibration
