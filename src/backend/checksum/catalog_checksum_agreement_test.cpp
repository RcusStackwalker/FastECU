// Every built-in protocol that corrects checksums and declares memory maps
// must keep each family's checksum stores in writable, ROM-file-backed memory
// of every one of those maps (ADR 0020, decision 5 of the slice 7 plan).
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_image.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/checksum/checksum_selection.h"
#include "src/backend/checksum/dispatch.h"
#include "src/backend/config/builtin_catalog.h"
#include "src/backend/config/catalog.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::checksum
{
namespace
{
using ::testing::Pair;
using ::testing::UnorderedElementsAre;
using Status = ChecksumCorrectionOutcome::Status;

TEST(CatalogChecksumAgreement, EveryDeclaredMapHoldsItsChecksumStoresInWritableMemory)
{
    std::set<std::string_view> seen;
    std::vector<std::pair<std::string, std::uint32_t>> bad_size;
    std::vector<std::pair<std::string, std::uint32_t>> layout_mismatch;
    int checked = 0;
    for (const config::VehicleSpec& vehicle : config::BuiltinCatalog().Vehicles())
    {
        const config::ProtocolSpec& protocol = *vehicle.protocol;
        if (protocol.checksum != config::ChecksumSupport::kCorrected || protocol.memory_maps.empty() ||
            !HasRoute(vehicle.make, protocol.name) || !seen.insert(protocol.name).second)
        {
            continue;
        }
        const FlashDevice *device = flash::FindFlashDevice(protocol.mcu);
        ASSERT_NE(device, nullptr) << protocol.name;
        for (const config::MemoryMapSpec& spec : protocol.memory_maps)
        {
            SCOPED_TRACE(std::format("{} {:#x}", protocol.name, spec.file_size.Value()));
            auto map = config::SelectMemoryMap(protocol, spec.file_size);
            ASSERT_TRUE(map.has_value());
            const auto image = memory::MemoryImage::Create(std::move(*map), bytes::Bytes(spec.file_size.Value(), 0));
            ASSERT_TRUE(image.has_value());

            const ChecksumCorrectionOutcome outcome =
                ApplyChecksumCorrection(*image, {.make = std::string(vehicle.make),
                                                 .checksum_flag = "yes",
                                                 .flash_method = std::string(protocol.name),
                                                 .mcu_type = std::string(protocol.mcu),
                                                 .rom_id = "39670016"});
            ++checked;
            if (spec.file_size.Value() != device->romsize)
            {
                EXPECT_EQ(outcome.status, Status::kBadRomSize);
                bad_size.emplace_back(protocol.name, spec.file_size.Value());
                continue;
            }
            ASSERT_EQ(outcome.status, Status::kFamilyRan);
            ASSERT_TRUE(outcome.family_result.has_value());
            if (outcome.family_result->status == ChecksumResult::Status::kInvalidSize)
            {
                layout_mismatch.emplace_back(protocol.name, spec.file_size.Value());
                continue;
            }
            EXPECT_TRUE(outcome.family_result->Ok()) << outcome.family_result->message;
            EXPECT_TRUE(outcome.corrected_file.has_value());
        }
    }

    EXPECT_GT(checked, 0);
    // N83M_1_5MB's romsize (0x174000) is not the 1.5M file size (0x184000), so
    // that route never runs (deferred).
    EXPECT_THAT(bad_size, UnorderedElementsAre(Pair("sub_ecu_denso_1n83m_1_5m_can", 0x184000U)));
    // sub_tcu_hitachi_m32r_can names M32R_512KB, but its family's layout is
    // 64 KiB, so no file of the MCU's romsize corrects (deferred).
    EXPECT_THAT(layout_mismatch, UnorderedElementsAre(Pair("sub_tcu_hitachi_m32r_can", 0x80000U)));
}
} // namespace
} // namespace fastecu::checksum
