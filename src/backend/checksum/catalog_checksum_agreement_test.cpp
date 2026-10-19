// Every built-in protocol that corrects checksums must correct a ROM of each
// file size it accepts, with every checksum store in writable, file-backed
// memory, and reject any other size (ADR 0020). It accepts the file sizes of
// the memory maps it declares, else its MCU's romsize under the identity map:
// the size gate before checksum routes declared their ECU window.
#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
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
using Status = ChecksumCorrectionOutcome::Status;

// A zero ROM is enough for most families to correct. The Mitsubishi M32R CAN
// family needs an area code at 0x3FFCB naming its sweep, or it is disabled and
// writes nothing: 0xC2 sweeps 0x60000 bytes and 0x50 sweeps 0x80000.
bytes::Bytes ZeroRomThatCorrects(std::string_view flash_method, std::uint32_t file_size)
{
    bytes::Bytes rom(file_size, 0);
    if (flash_method.starts_with("mitsu_ecu_m32r_can"))
    {
        rom[0x3FFCB] = file_size == 0x60000 ? 0xC2 : 0x50;
    }
    return rom;
}

// One vehicle per built-in protocol that corrects checksums through a route.
std::vector<const config::VehicleSpec *> OneVehiclePerRoutedProtocol()
{
    std::set<std::string_view> seen;
    std::vector<const config::VehicleSpec *> vehicles;
    for (const config::VehicleSpec& vehicle : config::BuiltinCatalog().Vehicles())
    {
        const config::ProtocolSpec& protocol = *vehicle.protocol;
        if (protocol.checksum == config::ChecksumSupport::kCorrected && HasRoute(vehicle.make, protocol.name) &&
            seen.insert(protocol.name).second)
        {
            vehicles.push_back(&vehicle);
        }
    }
    return vehicles;
}

std::vector<std::uint32_t> AcceptedFileSizes(const config::ProtocolSpec& protocol)
{
    auto sizes = protocol.memory_maps |
                 std::views::transform([](const config::MemoryMapSpec& spec) { return spec.FileSize().Value(); }) |
                 std::ranges::to<std::vector<std::uint32_t>>();
    if (sizes.empty())
    {
        if (const FlashDevice *device = flash::FindFlashDevice(protocol.mcu); device != nullptr)
        {
            sizes.push_back(device->romsize);
        }
    }
    return sizes;
}

// As CalibrationOperationCoordinator places the operation image: by the
// protocol's map for the size, else the identity map.
memory::MemoryImage PlaceAsTheCoordinatorDoes(const config::ProtocolSpec& protocol, const bytes::Bytes& rom)
{
    auto placed = config::PlaceRomFile(&protocol, rom);
    return placed.has_value() ? *std::move(placed) : config::PlaceRomFile(nullptr, rom).value();
}

ChecksumSelection SelectionFor(const config::VehicleSpec& vehicle)
{
    return {.make = std::string(vehicle.make),
            .checksum_flag = "yes",
            .flash_method = std::string(vehicle.protocol->name),
            .mcu_type = std::string(vehicle.protocol->mcu),
            .rom_id = "39670016"};
}

TEST(CatalogChecksumAgreement, EveryRoutedProtocolCorrectsEachFileSizeItAccepts)
{
    int checked = 0;
    for (const config::VehicleSpec *vehicle : OneVehiclePerRoutedProtocol())
    {
        const config::ProtocolSpec& protocol = *vehicle->protocol;
        const std::vector<std::uint32_t> sizes = AcceptedFileSizes(protocol);
        ASSERT_FALSE(sizes.empty()) << protocol.name;
        for (const std::uint32_t size : sizes)
        {
            SCOPED_TRACE(std::format("{} {:#x}", protocol.name, size));
            const ChecksumCorrectionOutcome outcome = ApplyChecksumCorrection(
                PlaceAsTheCoordinatorDoes(protocol, ZeroRomThatCorrects(protocol.name, size)), SelectionFor(*vehicle));
            ++checked;

            ASSERT_EQ(outcome.status, Status::kFamilyRan);
            ASSERT_TRUE(outcome.family_result.has_value());
            // Corrected, not merely Ok(): a family that writes nothing proves nothing.
            EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kCorrected)
                << outcome.family_result->message;
            EXPECT_TRUE(outcome.corrected_file.has_value());
        }
    }
    EXPECT_GT(checked, 0);
}

TEST(CatalogChecksumAgreement, EveryRoutedProtocolRejectsAFileOneWordLonger)
{
    for (const config::VehicleSpec *vehicle : OneVehiclePerRoutedProtocol())
    {
        const config::ProtocolSpec& protocol = *vehicle->protocol;
        for (const std::uint32_t size : AcceptedFileSizes(protocol))
        {
            SCOPED_TRACE(std::format("{} {:#x}", protocol.name, size + 4));
            const ChecksumCorrectionOutcome outcome = ApplyChecksumCorrection(
                PlaceAsTheCoordinatorDoes(protocol, bytes::Bytes(size + 4, 0)), SelectionFor(*vehicle));

            EXPECT_EQ(outcome.status, Status::kBadRomSize);
        }
    }
}
} // namespace
} // namespace fastecu::checksum
