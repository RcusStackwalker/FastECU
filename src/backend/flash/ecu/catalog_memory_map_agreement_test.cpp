// Every protocol that writes must agree with its flash family: the writable
// blocks of each memory map its catalog entry gives are exactly the addresses
// the family's own write plan writes, and they are flash in its FlashDevice.
// The catalog states the maps; the families keep their own windows (ADR 0019,
// ADR 0020). Neither may drift from the other.

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/config/builtin_catalog.h"
#include "src/backend/config/catalog.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_plan.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_types.h"

namespace fastecu::flash
{
namespace
{
using memory::ByteCount;
using memory::MemoryMap;

// How a family's write plan states the addresses it writes.
enum class WriteWindow
{
    // The plan's transfer region is the ECU address range it writes.
    kTransferRegion,
    // The plan's transfer region is in packed ROM file coordinates, which the
    // MC68HC16Y5 K-Line executor unpacks; the FlashDevice's flash blocks are
    // what it writes.
    kFlashBlocks,
    // The write uploads only a kernel (MC68HC16Y5 BDM); no ROM bytes.
    kNone,
};

using WritePlanBuilder = Result<FlashPlan> (*)(const config::ProtocolSpec& protocol, FlashOperation operation,
                                               bytes::Bytes image);
using ImageBuilder = Result<FlashPlan> (*)(FlashOperation, std::string_view, std::string_view,
                                           std::optional<bytes::Bytes>);
using KernelBuilder = Result<FlashPlan> (*)(FlashOperation, std::string_view, std::string_view,
                                            std::optional<bytes::Bytes>, KernelImage);

// The kernel the catalog names for `protocol`, with stand-in bytes.
KernelImage KernelFor(const config::ProtocolSpec& protocol)
{
    return KernelImage{.id = "kernel", .load_address = protocol.kernel_load_address.value_or(0U), .bytes = {0xAA}};
}

template <ImageBuilder kBuild>
Result<FlashPlan> ImageOnly(const config::ProtocolSpec& protocol, FlashOperation operation, bytes::Bytes image)
{
    return kBuild(operation, protocol.name, protocol.mcu, std::move(image));
}

template <KernelBuilder kBuild>
Result<FlashPlan> WithKernel(const config::ProtocolSpec& protocol, FlashOperation operation, bytes::Bytes image)
{
    return kBuild(operation, protocol.name, protocol.mcu, std::move(image), KernelFor(protocol));
}

Result<FlashPlan> UnisiaJecsM32rKline(const config::ProtocolSpec& protocol, FlashOperation operation,
                                      bytes::Bytes image)
{
    return BuildSubaruUnisiaJecsM32rKlinePlan(operation, protocol.name, protocol.mcu, std::move(image),
                                              /*adapter_supplies_programming_voltage=*/true);
}

struct AgreementRow
{
    std::string_view protocol;
    WritePlanBuilder build;
    WriteWindow window = WriteWindow::kTransferRegion;
};

// One row per built-in protocol that writes or test-writes.
constexpr auto kRows = std::to_array<AgreementRow>({
    {"sub_ecu_denso_sh7055_densocan", &WithKernel<&BuildSubaruDensoSh705xDensocanPlan>},
    {"sub_ecu_denso_sh7058_densocan", &WithKernel<&BuildSubaruDensoSh705xDensocanPlan>},
    {"sub_ecu_denso_sh7058s_densocan", &WithKernel<&BuildSubaruDensoSh705xDensocanPlan>},
    {"sub_ecu_denso_sh7058s_diesel_densocan", &WithKernel<&BuildSubaruDensoSh705xDensocanPlan>},
    {"sub_ecu_denso_sh7059_diesel_densocan", &WithKernel<&BuildSubaruDensoSh705xDensocanPlan>},
    {"sub_ecu_denso_mc68hc16y5_02_bdm", nullptr, WriteWindow::kNone},
    {"sub_ecu_denso_mc68hc16y5_02", &WithKernel<&BuildSubaruDensoMc68hc16y502Plan>, WriteWindow::kFlashBlocks},
    {"sub_ecu_denso_mc68hc16y5_02_ecutek", &WithKernel<&BuildSubaruDensoMc68hc16y502Plan>, WriteWindow::kFlashBlocks},
    {"sub_ecu_denso_sh7055_02", &WithKernel<&BuildSubaruDensoSh705502Plan>},
    {"sub_ecu_denso_sh7055_02_ecutek", &WithKernel<&BuildSubaruDensoSh705502Plan>},
    {"sub_ecu_denso_sh7055_04", &WithKernel<&BuildSubaruDensoSh705xKlinePlan>},
    {"sub_ecu_denso_sh7055_04_ecutek", &WithKernel<&BuildSubaruDensoSh705xKlinePlan>},
    {"sub_ecu_denso_sh7055_04_cobb", &WithKernel<&BuildSubaruDensoSh705xKlinePlan>},
    {"sub_ecu_denso_sh7058", &WithKernel<&BuildSubaruDensoSh705xKlinePlan>},
    {"sub_ecu_denso_sh7058_ecutek", &WithKernel<&BuildSubaruDensoSh705xKlinePlan>},
    {"sub_ecu_denso_sh7058_cobb", &WithKernel<&BuildSubaruDensoSh705xKlinePlan>},
    {"sub_ecu_denso_sh7058_can", &WithKernel<&BuildSubaruDensoSh7058CanPlan>},
    {"sub_ecu_denso_sh7058_can_ecutek", &WithKernel<&BuildSubaruDensoSh7058CanPlan>},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom", &WithKernel<&BuildSubaruDensoSh7058CanPlan>},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", &WithKernel<&BuildSubaruDensoSh7058CanPlan>},
    {"sub_ecu_denso_sh7058_can_cobb", &WithKernel<&BuildSubaruDensoSh7058CanPlan>},
    {"sub_ecu_denso_sh7058_can_diesel", &WithKernel<&BuildSubaruDensoSh7058CanDieselPlan>},
    {"sub_ecu_denso_sh7059_can_diesel", &WithKernel<&BuildSubaruDensoSh7058CanDieselPlan>},
    {"sub_ecu_denso_sh72543_can_diesel", &ImageOnly<&BuildSubaruDensoSh72543CanDieselPlan>},
    {"sub_ecu_unisia_jecs_20_bootmode", &ImageOnly<&BuildSubaruUnisiaJecsM32rBootmodeProgramPlan>},
    {"sub_ecu_unisia_jecs_20", &UnisiaJecsM32rKline},
    {"sub_ecu_unisia_jecs_30_bootmode", &ImageOnly<&BuildSubaruUnisiaJecsM32rBootmodeProgramPlan>},
    {"sub_ecu_unisia_jecs_30", &UnisiaJecsM32rKline},
    {"sub_ecu_hitachi_m32r_kline_recovery", &ImageOnly<&BuildSubaruHitachiM32rKlinePlan>},
    {"sub_ecu_hitachi_m32r_kline", &ImageOnly<&BuildSubaruHitachiM32rKlinePlan>},
    {"sub_ecu_hitachi_m32r_can", &ImageOnly<&BuildSubaruHitachiM32rCanPlan>},
    {"sub_tcu_hitachi_m32r_can", &ImageOnly<&BuildSubaruTcuHitachiM32rCanPlan>},
    {"sub_tcu_cvt_hitachi_m32r_can", &ImageOnly<&BuildSubaruTcuCvtHitachiM32rCanPlan>},
    {"sub_tcu_denso_sh7058_can", &WithKernel<&BuildSubaruTcuDensoSh705xCanPlan>},
    {"sub_tcu_cvt_mitsu_mh8104_can", &ImageOnly<&BuildSubaruTcuCvtMitsuMh8104CanPlan>},
    {"sub_tcu_cvt_mitsu_mh8111_can", &ImageOnly<&BuildSubaruTcuCvtMitsuMh8111CanPlan>},
    {"sub_ecu_mitsu_m32r_kline", &ImageOnly<&BuildSubaruMitsuM32rKlinePlan>},
    {"mitsu_ecu_m32r_can", &ImageOnly<&BuildMitsuColtM32rCanPlan>},
    {"mitsu_ecu_m32r_can_vendor_ext", &ImageOnly<&BuildMitsuColtM32rCanPlan>},
    {"mitsu_ecu_m32r_can_512kb", &ImageOnly<&BuildMitsuColtM32rCanPlan>},
    {"mitsu_ecu_m32r_can_vendor_ext_512kb", &ImageOnly<&BuildMitsuColtM32rCanPlan>},
    {"sub_ecu_hitachi_sh7058_can", &ImageOnly<&BuildSubaruHitachiSh7058Plan>},
    {"sub_ecu_hitachi_sh72543r_can", &ImageOnly<&BuildSubaruHitachiSh72543rCanPlan>},
    {"sub_ecu_hitachi_sh72543r_can_recovery", &ImageOnly<&BuildSubaruHitachiSh72543rCanPlan>},
    {"sub_ecu_denso_sh72531_can", &ImageOnly<&BuildSubaruDensoSh72531CanPlan>},
    {"sub_ecu_denso_1n83m_4m_can", &ImageOnly<&BuildSubaruDenso1n83m4mCanPlan>},
    {"sub_ecu_denso_1n83m_1_5m_can", &ImageOnly<&BuildSubaruDenso1n83m15mCanPlan>},
});

// A half-open address range [start, end), wide enough that no end overflows.
struct Extent
{
    std::uint64_t start;
    std::uint64_t end;

    bool operator==(const Extent&) const = default;
};

void PrintTo(const Extent& extent, std::ostream *out)
{
    *out << std::format("[0x{:x}, 0x{:x})", extent.start, extent.end);
}

// Sorted, with overlapping or touching extents joined. SH72531's FlashDevice
// lists a third block that overlaps its second, so overlaps must join too.
std::vector<Extent> Merged(std::vector<Extent> extents)
{
    std::ranges::sort(extents, {}, &Extent::start);
    std::vector<Extent> merged;
    for (const Extent& extent : extents)
    {
        if (!merged.empty() && extent.start <= merged.back().end)
        {
            merged.back().end = std::max(merged.back().end, extent.end);
        }
        else
        {
            merged.push_back(extent);
        }
    }
    return merged;
}

Extent ExtentOf(const memory::MemoryBlock& block)
{
    return Extent{.start = block.range.Start().Value(), .end = block.range.End().Value()};
}

Extent ExtentOf(const MemoryRegion& region)
{
    return Extent{.start = region.start, .end = std::uint64_t{region.start} + region.length};
}

std::vector<Extent> WritableExtents(const MemoryMap& map)
{
    std::vector<Extent> extents;
    for (const memory::MemoryBlock& block : map.Blocks())
    {
        if (block.writability == memory::Writability::kWritable)
        {
            extents.push_back(ExtentOf(block));
        }
    }
    return Merged(std::move(extents));
}

std::vector<Extent> FlashExtents(const FlashDevice& device)
{
    std::vector<Extent> extents;
    for (const FlashBlock& block : std::span(device.fblocks, device.numblocks))
    {
        extents.push_back(Extent{.start = block.start, .end = std::uint64_t{block.start} + block.len});
    }
    return Merged(std::move(extents));
}

bool Within(const Extent& inner, const std::vector<Extent>& outer)
{
    return std::ranges::any_of(outer, [&inner](const Extent& extent)
                               { return extent.start <= inner.start && inner.end <= extent.end; });
}

bool Overlaps(const Extent& inner, const std::vector<Extent>& outer)
{
    return std::ranges::any_of(outer, [&inner](const Extent& extent)
                               { return extent.start < inner.end && inner.start < extent.end; });
}

bool Writes(const config::ProtocolSpec& protocol)
{
    return protocol.write || protocol.test_write;
}

const AgreementRow *RowFor(std::string_view protocol)
{
    const auto found = std::ranges::find(kRows, protocol, &AgreementRow::protocol);
    return found == kRows.end() ? nullptr : &*found;
}

// The memory maps a ROM file for `protocol` can have: one per declared map, or
// for a protocol that declares none, the identity map over the FlashDevice's
// ROM size.
std::vector<MemoryMap> CandidateMaps(const config::ProtocolSpec& protocol, const FlashDevice& device)
{
    std::vector<ByteCount> sizes;
    if (protocol.memory_maps.empty())
    {
        sizes.push_back(ByteCount{device.romsize});
    }
    for (const config::MemoryMapSpec& spec : protocol.memory_maps)
    {
        sizes.push_back(spec.file_size);
    }
    std::vector<MemoryMap> maps;
    for (const ByteCount size : sizes)
    {
        auto map = config::SelectMemoryMap(protocol, size);
        if (map.has_value())
        {
            maps.push_back(std::move(*map));
        }
        else
        {
            ADD_FAILURE() << protocol.name << ": " << map.error().detail;
        }
    }
    return maps;
}

TEST(CatalogMemoryMapAgreement, EveryProtocolThatWritesHasARowAndEveryRowNamesOne)
{
    for (const config::ProtocolSpec& protocol : config::BuiltinCatalog().Protocols())
    {
        if (Writes(protocol))
        {
            EXPECT_NE(RowFor(protocol.name), nullptr) << protocol.name << " writes but has no agreement row";
        }
    }
    for (const AgreementRow& row : kRows)
    {
        const config::ProtocolSpec *protocol = config::BuiltinCatalog().FindProtocol(row.protocol);
        EXPECT_TRUE(protocol != nullptr && Writes(*protocol))
            << row.protocol << " is not a built-in protocol that writes";
    }
}

TEST(CatalogMemoryMapAgreement, WritePlansWriteExactlyTheWritableBlocks)
{
    for (const AgreementRow& row : kRows)
    {
        const config::ProtocolSpec *protocol = config::BuiltinCatalog().FindProtocol(row.protocol);
        ASSERT_NE(protocol, nullptr) << row.protocol;
        const FlashDevice *device = FindFlashDevice(protocol->mcu);
        ASSERT_NE(device, nullptr) << row.protocol << " names no FlashDevice: " << protocol->mcu;
        if (row.window == WriteWindow::kNone)
        {
            continue;
        }
        const FlashOperation operation = protocol->write ? FlashOperation::kWrite : FlashOperation::kTestWrite;
        int accepted = 0;
        for (const MemoryMap& map : CandidateMaps(*protocol, *device))
        {
            const Result<FlashPlan> plan = row.build(*protocol, operation, bytes::Bytes(map.FileSize().Value(), 0xFF));
            if (!plan.has_value())
            {
                continue; // the family takes ROM files of another declared size
            }
            ++accepted;
            const std::vector<Extent> writable = WritableExtents(map);
            const std::vector<Extent> expected = row.window == WriteWindow::kTransferRegion
                                                     ? std::vector{ExtentOf(plan->TransferRegion())}
                                                     : FlashExtents(*device);
            EXPECT_EQ(writable, expected) << row.protocol << ", " << map.FileSize().Value() << "-byte ROM file";
            if (row.window == WriteWindow::kTransferRegion)
            {
                for (const MemoryRegion& erase : plan->EraseRegions())
                {
                    EXPECT_TRUE(Within(ExtentOf(erase), writable))
                        << row.protocol << " erases outside its writable blocks";
                }
            }
        }
        EXPECT_GT(accepted, 0) << row.protocol << " accepts no ROM file its memory maps describe";
    }
}

TEST(CatalogMemoryMapAgreement, WritableBlocksAreFlashAndFillBlocksAreNot)
{
    for (const config::ProtocolSpec& protocol : config::BuiltinCatalog().Protocols())
    {
        if (!Writes(protocol))
        {
            continue;
        }
        const FlashDevice *device = FindFlashDevice(protocol.mcu);
        ASSERT_NE(device, nullptr) << protocol.name;
        const std::vector<Extent> flash = FlashExtents(*device);
        for (const MemoryMap& map : CandidateMaps(protocol, *device))
        {
            for (const memory::MemoryBlock& block : map.Blocks())
            {
                if (block.writability == memory::Writability::kWritable)
                {
                    EXPECT_TRUE(Within(ExtentOf(block), flash)) << protocol.name << ": writable block at 0x" << std::hex
                                                                << block.range.Start().Value() << " is not flash";
                }
                if (std::holds_alternative<memory::FillBacking>(block.backing))
                {
                    EXPECT_FALSE(Overlaps(ExtentOf(block), flash)) << protocol.name << ": fill block at 0x" << std::hex
                                                                   << block.range.Start().Value() << " hides flash";
                }
            }
        }
    }
}
} // namespace
} // namespace fastecu::flash
