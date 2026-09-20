// subaru_tcu_hitachi_m32r_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

#include <string>
#include <utility>

#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash::testing
{
namespace
{

// Independently transcribed from subaru_tcu_hitachi_m32r_can_plan.cpp's
// anonymous-namespace constants (not visible outside the .cpp) -- these
// tests exist specifically to reach validate_subaru_tcu_hitachi_m32r_can_plan
// by direct construction, bypassing the builder entirely, so the builder's
// own use of matching literals proves nothing about the validator in
// isolation.
constexpr std::string_view kProtocol = "sub_tcu_hitachi_m32r_can";
constexpr std::string_view kMcuName = "M32R_512KB";
constexpr MemoryRegion kWindow{.start = 0x8000, .length = 0x78000};
constexpr std::uint32_t kImageSize = 0x80000;

FlashPlanFields valid_fields(FlashOperation operation = FlashOperation::Write)
{
    return {
        .operation = operation,
        .family = FlashFamily::SubaruTcuHitachiM32rCan,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(kProtocol),
        .mcu_name = std::string(kMcuName),
        .transfer_region = kWindow,
        .erase_regions = operation == FlashOperation::Read ? std::vector<MemoryRegion>{} : std::vector{kWindow},
        .image = operation == FlashOperation::Read ? std::nullopt
                                                   : std::optional<bytes::Bytes>(bytes::Bytes(kImageSize, 0x00)),
        .kernel = std::nullopt,
        .family_plan = SubaruTcuHitachiM32rCanPlan{.request_id = 0x7E1,
                                                   .response_id = 0x7E9,
                                                   .bitrate = 500000,
                                                   .extended_id = false,
                                                   .page_size = 0x100,
                                                   .write_frame_size = 128},
        .confirmations = {},
    };
}
constexpr SingleWindowPlanCase kCase{
    .name = "SubaruTcuHitachiM32rCan",
    .build = &build_subaru_tcu_hitachi_m32r_can_plan,
    .protocol = "sub_tcu_hitachi_m32r_can",
    .mcu = "M32R_512KB",
    .foreign_protocol = "sub_tcu_hitachi_m32r_can_typo",
    .foreign_mcu = "M32R_512KB_1block",
    // Legacy read_mem computes start_addr - 0x00100000 with start_addr == 0,
    // which underflows uint32_t to 0xFFF00000 and bypasses the "< 0x8000"
    // floor clamp entirely -- see subaru_tcu_hitachi_m32r_can_types.h's
    // divergence 2. This plan targets the clamp's evident intent (0x8000)
    // instead of the never-observed underflowed address.
    .read_region = MemoryRegion{.start = 0x8000, .length = 0x78000},
    .erase_region = MemoryRegion{.start = 0x8000, .length = 0x78000},
    .image_size = 0x80000,
};

INSTANTIATE_TEST_SUITE_P(SubaruTcuHitachiM32rCan, SingleWindowPlanContract, ::testing::Values(kCase), caseName);

// The wire parameters are this family's own; they do not generalize.
TEST(SubaruTcuHitachiM32rCanPlan, ReadPlanCarriesThisFamilysWireParameters)
{
    const auto plan = build_subaru_tcu_hitachi_m32r_can_plan(FlashOperation::Read, "sub_tcu_hitachi_m32r_can",
                                                             "M32R_512KB", std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto& family = std::get<SubaruTcuHitachiM32rCanPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7E1U);
    EXPECT_EQ(family.response_id, 0x7E9U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_EQ(family.page_size, 0x100U);
    EXPECT_EQ(family.write_frame_size, 128U);
}

// Deliberate divergence 1, and the safety-critical assertion of this whole
// task: the legacy reflash_block ignores its test_write_arg parameter
// entirely (see subaru_tcu_hitachi_m32r_can_types.h), so "test write" would
// perform the same live erase and flash write as "write" if ported as-is.
// This plan has no dry-run to port and rejects TestWrite outright.
// SingleWindowPlanContract.TestWriteIsRejectedBeforeAnyIo already covers
// this (unconditionally, regardless of supports_write); this test restates
// it directly because it is the requirement the whole task exists to meet.
TEST(SubaruTcuHitachiM32rCanPlan, RejectsTestWriteAsUnsupported)
{
    const auto plan = build_subaru_tcu_hitachi_m32r_can_plan(FlashOperation::TestWrite, "sub_tcu_hitachi_m32r_can",
                                                             "M32R_512KB", bytes::Bytes(0x80000, 0x00));
    EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::Unsupported));
}

// Second line of defense: the builder intercepts TestWrite before a FlashPlan
// is ever constructed, so nothing above exercises the standalone validator's
// own TestWrite branch. Anything that reaches validate_subaru_tcu_hitachi_m32r_can_plan
// without coming through the builder -- which is exactly what Task 2's
// executor does on every transport_setup()/execute() call -- must be
// rejected by the validator itself, independent of the builder. Constructs
// FlashPlanFields directly and goes through validate_and_build (which has no
// TestWrite policy of its own and is expected to succeed) to prove that.
TEST(SubaruTcuHitachiM32rCanPlan, ValidatorRejectsTestWriteDirectlyConstructed)
{
    auto fields = valid_fields(FlashOperation::TestWrite);
    auto built = validate_and_build(std::move(fields));
    ASSERT_THAT(built, fastecu::testing::IsOk());
    EXPECT_THAT(validate_subaru_tcu_hitachi_m32r_can_plan(*built), fastecu::testing::IsErr(ErrorKind::Unsupported));
}

// Direct-construction mutations proving the validator's own checks are
// pinned rather than only ever reached through the builder's matching
// literals. Mirrors subaru_denso_sh7058_can_plan_test.cpp's
// ValidatorRejectsWireSecurityRegionGeometryAndConfirmationDrift.
TEST(SubaruTcuHitachiM32rCanPlan, ValidatorRejectsWireRegionAndImageDrift)
{
    for (int mutation = 0; mutation < 5; ++mutation)
    {
        auto fields = valid_fields();
        auto& wire = std::get<SubaruTcuHitachiM32rCanPlan>(fields.family_plan);
        switch (mutation)
        {
        case 0:
            // Transposed request/response ids.
            std::swap(wire.request_id, wire.response_id);
            break;
        case 1:
            wire.bitrate = 250000;
            break;
        case 2:
            fields.transfer_region.length -= 1U;
            break;
        case 3:
            fields.image = bytes::Bytes(kImageSize - 1U, 0x00);
            break;
        case 4:
            fields.image = bytes::Bytes(kImageSize + 1U, 0x00);
            break;
        default:
            FAIL() << "unexpected validator mutation " << mutation;
        }
        auto built = validate_and_build(std::move(fields));
        ASSERT_THAT(built, fastecu::testing::IsOk()) << mutation;
        auto valid = validate_subaru_tcu_hitachi_m32r_can_plan(*built);
        EXPECT_THAT(valid, fastecu::testing::IsErr(ErrorKind::InvalidConfig)) << mutation;
    }
}
} // namespace
} // namespace fastecu::flash::testing
