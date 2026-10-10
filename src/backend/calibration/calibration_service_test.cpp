#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/calibration/calibration_service.h"

#include <format>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "src/backend/ports/testing/in_memory_file_repository.h"

namespace fastecu::calibration
{
namespace
{

using fastecu::InMemoryFileRepository;
using fastecu::definition::AxisDefinition;
using fastecu::definition::CalibrationMap;
using fastecu::definition::RomDefinition;
using fastecu::definition::Scaling;
using fastecu::definition::StorageType;

TEST(ReadRom, ReadsRequestedHandleThroughRepository)
{
    InMemoryFileRepository repo;
    repo.files["in.bin"] = {0xAA, 0xBB};

    ASSERT_THAT(ReadRom("in.bin", repo), fastecu::testing::IsOkAnd((std::vector<std::uint8_t>{0xAA, 0xBB})));
}

TEST(ReadRom, ReadFailureIsPropagated)
{
    InMemoryFileRepository repo;

    ASSERT_THAT(ReadRom("missing.bin", repo), ::testing::Not(fastecu::testing::IsOk()));
}

TEST(ReadRom, EmptyRomIsAValidResultNotAMissingOne)
{
    // The two-mode open_rom this replaced used span emptiness to pick its
    // mode, which conflated "no preloaded bytes" with "a zero-length ROM".
    // read_rom has one mode, so a zero-length file is just a zero-length
    // successful read.
    InMemoryFileRepository repo;
    repo.files["empty.bin"] = {};

    Result<std::vector<std::uint8_t>> result = ReadRom("empty.bin", repo);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(result->empty());
}

TEST(BackupRom, WritesBytesToBackupHandle)
{
    InMemoryFileRepository repo;
    const std::vector<std::uint8_t> rom_data{0x01, 0x02, 0x03, 0x04};

    BackupRom(rom_data, "backup.bin", repo);

    EXPECT_EQ(repo.files["backup.bin"], rom_data);
    EXPECT_EQ(repo.ReadCount("backup.bin"), 0);
}

TEST(BackupRom, WriteFailureIsSwallowed)
{
    // The whole reason backup_rom returns void: open_subaru_rom_file never
    // inspected this write's result, so a failed backup must not be able to
    // fail the open.
    class FailingBackupRepository : public InMemoryFileRepository
    {
      public:
        Status Write(std::string_view, std::span<const std::uint8_t>) override
        {
            return fastecu::Fail(ErrorKind::kInternal, "backup failed");
        }
    } repo;
    const std::vector<std::uint8_t> rom_data{0x01};

    BackupRom(rom_data, "backup.bin", repo);

    EXPECT_TRUE(repo.files.find("backup.bin") == repo.files.end());
}

TEST(ElementByteSizeTest, DelegatesToStorageByteSizeWhenNotBloblist)
{
    EXPECT_EQ(ElementByteSize(StorageType::kUint16, nullptr), 2U);
    EXPECT_EQ(ElementByteSize(std::optional<StorageType>{}, nullptr), 1U);
}

TEST(ElementByteSizeTest, BloblistWithNoScalingFallsBackToOneByte)
{
    EXPECT_EQ(ElementByteSize(StorageType::kBloblist, nullptr), 1U);
}

TEST(ElementByteSizeTest, BloblistWithEmptySelectionsFallsBackToOneByte)
{
    Scaling scaling;
    EXPECT_EQ(ElementByteSize(StorageType::kBloblist, &scaling), 1U);
}

TEST(ElementByteSizeTest, BloblistWidthComesFromFirstSelectionLength)
{
    Scaling scaling;
    scaling.selections = {{"disabled", {0x00, 0x00}}, {"enabled", {0x00, 0x01}}};

    EXPECT_EQ(ElementByteSize(StorageType::kBloblist, &scaling), 2U);
}

TEST(ElementByteSizeTest, BloblistWidthMatchesLegacySingleByteSelections)
{
    Scaling scaling;
    scaling.selections = {{"disabled", {0x00}}, {"enabled", {0x01}}};

    EXPECT_EQ(ElementByteSize(StorageType::kBloblist, &scaling), 1U);
}

TEST(ElementRunEndTest, SingleElementIsAddressPlusWidth)
{
    EXPECT_EQ(ElementRunEnd(0x1000, /*start_position=*/1, /*interval=*/1,
                            /*element_width=*/4, /*count=*/1),
              0x1004U);
}

TEST(ElementRunEndTest, ContiguousRunMatchesCountTimesWidth)
{
    // address=0x100, start_position=1, interval=1, width=2, count=4:
    // elements at 0x100, 0x102, 0x104, 0x106; last occupies [0x106, 0x108).
    EXPECT_EQ(ElementRunEnd(0x100, 1, 1, 2, 4), 0x108U);
}

TEST(ElementRunEndTest, StridedRunMatchesLegacyPerCellAddressFormula)
{
    // address=0x200, start_position=3, interval=2, width=1, count=3.
    // Legacy: addr(j) = address + (start_position-1)*width + j*width*interval.
    // addr(0)=0x202, addr(1)=0x204, addr(2)=0x206; last occupies [0x206, 0x207).
    EXPECT_EQ(ElementRunEnd(0x200, 3, 2, 1, 3), 0x207U);
}

TEST(ElementRunEndTest, ZeroStartPositionIsTreatedAsTheFirstPosition)
{
    // start_position is 1-based and unvalidated upstream, so startpos="0"
    // reaches here. Computing 0 - 1 in uint32 would wrap to 0xFFFFFFFF and
    // put the run ~4 GB past `address`.
    EXPECT_EQ(ElementRunEnd(0x1000, /*start_position=*/0, /*interval=*/1,
                            /*element_width=*/4, /*count=*/1),
              ElementRunEnd(0x1000, 1, 1, 4, 1));
    EXPECT_EQ(ElementRunEnd(0x1000, 0, 1, 4, 1), 0x1004U);
}

TEST(ElementRunEndTest, ZeroStartPositionDoesNotWrapWithAMultiElementRun)
{
    EXPECT_EQ(ElementRunEnd(0x100, /*start_position=*/0, /*interval=*/2,
                            /*element_width=*/2, /*count=*/3),
              0x10AU);
}

TEST(ElementRunEndTest, ZeroCountTouchesNothingPastTheAddress)
{
    // count - 1 would wrap the same way. An empty run ends where it starts.
    EXPECT_EQ(ElementRunEnd(0x1000, /*start_position=*/1, /*interval=*/1,
                            /*element_width=*/4, /*count=*/0),
              0x1000U);
    EXPECT_EQ(ElementRunEnd(0x1000, /*start_position=*/8, /*interval=*/3,
                            /*element_width=*/4, /*count=*/0),
              0x1000U);
}

TEST(ElementRunEndTest, ZeroCountAndZeroStartPositionTogetherDoNotWrap)
{
    EXPECT_EQ(ElementRunEnd(0x1000, 0, 1, 4, 0), 0x1000U);
}

namespace
{
// A RomDefinition with one 3x1 uint8 map named "Fuel" at `address`, scaled by
// `expression`.
definition::RomDefinition OneMapDefinition(std::uint64_t address, std::string_view expression = "x")
{
    definition::RomDefinition rom;
    rom.scalings.push_back(definition::Scaling{.name = "FuelScaling", .from_byte = std::string(expression)});
    definition::CalibrationMap map;
    map.name = "Fuel";
    map.type = "2D";
    map.address = address;
    map.x_size = 3;
    map.y_size = 1;
    map.storage_type = definition::StorageType::kUint8;
    map.endian = "big";
    map.scaling_name = "FuelScaling";
    rom.maps.push_back(map);
    return rom;
}
} // namespace

TEST(DecodeCalibrationMap, HasExactExtentAndExplicitAbsentAxes)
{
    auto definition = OneMapDefinition(0);
    definition.maps[0].x_size = 4;
    const bytes::Bytes rom{1, 2, 3, 4};
    const auto decoded = DecodeCalibrationMap(definition, definition.maps[0], rom);
    ASSERT_THAT(decoded, fastecu::testing::IsOk());
    EXPECT_THAT(std::get<NumericRun>(decoded->body).cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(1), fastecu::testing::IsOkAnd(2),
                                       fastecu::testing::IsOkAnd(3), fastecu::testing::IsOkAnd(4)));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(decoded->x_axis));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(decoded->y_axis));
}

TEST(DecodeCalibrationMap, UsesIdentityWithoutScaling)
{
    auto definition = OneMapDefinition(0);
    definition.scalings.clear();
    definition.maps[0].scaling_name.clear();
    const bytes::Bytes rom{5, 6, 7};
    const auto decoded = DecodeCalibrationMap(definition, definition.maps[0], rom);
    ASSERT_THAT(decoded, fastecu::testing::IsOk());
    EXPECT_THAT(std::get<NumericRun>(decoded->body).cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(5), fastecu::testing::IsOkAnd(6),
                                       fastecu::testing::IsOkAnd(7)));
}

TEST(DecodeCalibrationMap, RetainsLabelsContainingCommas)
{
    auto definition = OneMapDefinition(0);
    definition.maps[0].x_size = 2;
    definition.maps[0].x_axis.type = "Static X Axis";
    definition.maps[0].x_axis.static_data = {"Low, load", "High"};
    const bytes::Bytes rom{1, 2};
    const auto decoded = DecodeCalibrationMap(definition, definition.maps[0], rom);
    ASSERT_THAT(decoded, fastecu::testing::IsOk());
    EXPECT_THAT(std::get<StaticAxis>(decoded->x_axis).labels, ::testing::ElementsAre("Low, load", "High"));
}

TEST(DecodeCalibrationMap, RetainsBlobBytes)
{
    auto definition = OneMapDefinition(0);
    definition.maps[0].storage_type = StorageType::kBloblist;
    definition.scalings[0].selections = {{"Choice", {0xcc, 0xdd}}};
    const bytes::Bytes rom{0xcc, 0xdd};
    const auto decoded = DecodeCalibrationMap(definition, definition.maps[0], rom);
    ASSERT_THAT(decoded, fastecu::testing::IsOk());
    EXPECT_THAT(std::get<BlobValue>(decoded->body).data, ::testing::ElementsAre(0xcc, 0xdd));
}

TEST(DecodeCalibrationMap, KeepsComputationFailureLocalToCell)
{
    auto definition = OneMapDefinition(0, "1/x");
    definition.maps[0].x_size = 2;
    const bytes::Bytes rom{0, 2};
    const auto decoded = DecodeCalibrationMap(definition, definition.maps[0], rom);
    ASSERT_THAT(decoded, fastecu::testing::IsOk());
    EXPECT_THAT(
        std::get<NumericRun>(decoded->body).cells,
        ::testing::ElementsAre(fastecu::testing::IsErr(ErrorKind::kInvalidConfig), fastecu::testing::IsOkAnd(0.5)));
}

TEST(DecodeCalibrationMap, RejectsStructuralFailures)
{
    auto definition = OneMapDefinition(0);
    const bytes::Bytes rom{1, 2, 3};
    auto& map = definition.maps[0];
    map.address.reset();
    EXPECT_THAT(DecodeCalibrationMap(definition, map, rom), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    map.address = 0;
    map.x_size = 0;
    EXPECT_THAT(DecodeCalibrationMap(definition, map, rom), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    map.x_size = std::numeric_limits<std::uint32_t>::max();
    map.y_size = 2;
    EXPECT_THAT(DecodeCalibrationMap(definition, map, rom), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    map.x_size = 3;
    map.y_size = 1;
    map.address = 1;
    EXPECT_THAT(DecodeCalibrationMap(definition, map, rom), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

struct TypedStorageCase
{
    StorageType storage;
    std::string_view endian;
    bytes::Bytes bytes;
    double expected;
};

class DecodeNumericStorage : public ::testing::TestWithParam<TypedStorageCase>
{
};

TEST_P(DecodeNumericStorage, DecodesRawNumericRepresentation)
{
    const auto& example = GetParam();
    const ElementRun run{.count = 1, .storage_type = example.storage, .endian = example.endian};
    const auto decoded = DecodeNumericRun(example.bytes, run);
    ASSERT_THAT(decoded, fastecu::testing::IsOk());
    EXPECT_THAT(decoded->cells, ::testing::ElementsAre(fastecu::testing::IsOkAnd(example.expected)));
}

INSTANTIATE_TEST_SUITE_P(
    WidthsAndByteOrders, DecodeNumericStorage,
    ::testing::Values(TypedStorageCase{StorageType::kUint8, "big", {0xff}, 255},
                      TypedStorageCase{StorageType::kInt8, "big", {0xff}, -1},
                      TypedStorageCase{StorageType::kUint16, "big", {0x12, 0x34}, 4660},
                      TypedStorageCase{StorageType::kInt16, "little", {0x00, 0x80}, -32768},
                      TypedStorageCase{StorageType::kUint24, "little", {0xff, 0xff, 0xff}, 16777215},
                      TypedStorageCase{StorageType::kInt24, "big", {0x80, 0, 0}, -8388608},
                      TypedStorageCase{StorageType::kUint32, "big", {0xff, 0xff, 0xff, 0xff}, 4294967295.0},
                      TypedStorageCase{StorageType::kInt32, "little", {0, 0, 0, 0x80}, -2147483648.0},
                      TypedStorageCase{StorageType::kFloat, "little", {0x3f, 0xc0, 0, 0}, 1.5}));

TEST(DecodeNumericRun, RejectsOverflowingLayoutsBeforeReading)
{
    const bytes::Bytes rom{42};
    ElementRun run{
        .address = std::numeric_limits<std::uint64_t>::max(), .count = 1, .storage_type = StorageType::kUint8};
    EXPECT_FALSE(DecodeNumericRun(rom, run).has_value());
    run.start_position = 2;
    EXPECT_FALSE(DecodeNumericRun(rom, run).has_value());
    run.address = std::numeric_limits<std::uint64_t>::max() - 3;
    run.start_position = 2;
    run.storage_type = StorageType::kUint32;
    EXPECT_FALSE(DecodeNumericRun(rom, run).has_value());
}

TEST(ElementRunEndTest, RejectsOverflowingStrideProduct)
{
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    EXPECT_EQ(ElementRunEnd(0, 1, maximum, 4, maximum), std::numeric_limits<std::uint64_t>::max());
}

TEST(DecodeNumericRun, PreservesStrideAndBlankIdentity)
{
    const bytes::Bytes rom{99, 2, 99, 4};
    const ElementRun run{
        .count = 2, .start_position = 2, .interval = 2, .storage_type = StorageType::kUint8, .from_byte = " "};
    const auto decoded = DecodeNumericRun(rom, run);
    ASSERT_THAT(decoded, fastecu::testing::IsOk());
    EXPECT_THAT(decoded->cells, ::testing::ElementsAre(fastecu::testing::IsOkAnd(2), fastecu::testing::IsOkAnd(4)));
}

TEST(DecodeNumericRun, RetainsNonFiniteStorageAsCellError)
{
    const bytes::Bytes rom{0x7f, 0xc0, 0, 0};
    const ElementRun run{.count = 1, .storage_type = StorageType::kFloat};
    const auto decoded = DecodeNumericRun(rom, run);
    ASSERT_THAT(decoded, fastecu::testing::IsOk());
    EXPECT_THAT(decoded->cells, ::testing::ElementsAre(fastecu::testing::IsErr(ErrorKind::kInvalidConfig)));
}

TEST(DecodeCalibrationMap, UsesMapThenScalingStoragePrecedence)
{
    auto definition = OneMapDefinition(0);
    auto& map = definition.maps[0];
    map.x_size = 1;
    map.storage_type.reset();
    map.endian.clear();
    definition.scalings[0].storage_type = StorageType::kUint16;
    definition.scalings[0].endian = "little";
    const bytes::Bytes rom{0x34, 0x12};
    const auto inherited = DecodeCalibrationMap(definition, map, rom);
    ASSERT_THAT(inherited, fastecu::testing::IsOk());
    EXPECT_THAT(std::get<NumericRun>(inherited->body).cells, ::testing::ElementsAre(fastecu::testing::IsOkAnd(4660)));
    map.storage_type = StorageType::kUint8;
    const auto overridden = DecodeCalibrationMap(definition, map, rom);
    ASSERT_THAT(overridden, fastecu::testing::IsOk());
    EXPECT_THAT(std::get<NumericRun>(overridden->body).cells, ::testing::ElementsAre(fastecu::testing::IsOkAnd(52)));
}

TEST(DecodeCalibrationMap, DecodesNumericAxesWithResolvedExpressions)
{
    auto definition = OneMapDefinition(0);
    auto& map = definition.maps[0];
    map.x_size = 2;
    map.y_size = 2;
    map.x_axis =
        AxisDefinition{.type = "X Axis", .storage_type = StorageType::kUint8, .address = 4, .from_byte = "x*2"};
    map.y_axis =
        AxisDefinition{.type = "Y Axis", .storage_type = StorageType::kUint8, .address = 6, .from_byte = "x/2"};
    const bytes::Bytes rom{1, 2, 3, 4, 5, 6, 8, 10};
    const auto decoded = DecodeCalibrationMap(definition, map, rom);
    ASSERT_THAT(decoded, fastecu::testing::IsOk());
    EXPECT_THAT(std::get<NumericRun>(decoded->x_axis).cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(10), fastecu::testing::IsOkAnd(12)));
    EXPECT_THAT(std::get<NumericRun>(decoded->y_axis).cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(4), fastecu::testing::IsOkAnd(5)));
}

TEST(DecodeNumericRun, AppliesStrideInStorageBytes)
{
    const bytes::Bytes rom{99, 99, 0x12, 0x34, 99, 99, 99, 99, 0x56, 0x78};
    const ElementRun run{.count = 2, .start_position = 2, .interval = 3, .storage_type = StorageType::kUint16};
    const auto decoded = DecodeNumericRun(rom, run);
    ASSERT_THAT(decoded, fastecu::testing::IsOk());
    EXPECT_THAT(decoded->cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(4660), fastecu::testing::IsOkAnd(22136)));
}

} // namespace
} // namespace fastecu::calibration
