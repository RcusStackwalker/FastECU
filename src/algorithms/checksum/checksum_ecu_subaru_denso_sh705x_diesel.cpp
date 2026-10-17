#include "checksum_ecu_subaru_denso_sh705x_diesel.h"

#include "denso_checksum_table.h"

#include <array>

ChecksumResult ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(const fastecu::memory::MemoryView& rom,
                                                                           fastecu::memory::FlashAddress table_address,
                                                                           uint32_t table_length)
{
    const bytes::ByteView rom_view = rom.Data();
    const fastecu::memory::FlashAddress base = rom.Range().Start();
    const std::uint32_t checksum_area_start = table_address.Value();
    using fastecu::checksum::internal::DensoTableOutcome;
    using fastecu::checksum::internal::DensoTableSpec;
    using fastecu::checksum::internal::DensoWordOverride;

    ChecksumResult result;
    result.rom_data.assign(rom_view.begin(), rom_view.end());
    std::array<DensoWordOverride, 1> overrides{};
    std::span<const DensoWordOverride> active_overrides;
    if (checksum_area_start == 0x0FFB80)
    {
        overrides[0] = {0x0FFAFC, 0xFFFFFFFF};
        active_overrides = overrides;
    }
    else if (checksum_area_start == 0x17FB80)
    {
        overrides[0] = {0x17FAFC, 0xFFFFFFFF};
        active_overrides = overrides;
    }

    const DensoTableSpec primary{
        .table_address = table_address,
        .table_length = table_length,
        .overrides = active_overrides,
    };
    const DensoTableOutcome primary_outcome =
        fastecu::checksum::internal::CorrectDensoTable(base, result.rom_data, primary);
    if (primary_outcome == DensoTableOutcome::kDisabled)
    {
        result.status = ChecksumResult::Status::kDisabled;
        // Disabled is a successful status, so the adapter propagates romData
        // back to FullRomData. Keep the original bytes here; the historical
        // empty return value could otherwise wipe the loaded ROM.
        result.message = "ROM has all checksums disabled";
        return result;
    }
    if (const auto failure = fastecu::checksum::internal::FailureOf(primary_outcome))
    {
        result.status = failure->status;
        result.message = failure->message;
        return result;
    }

    DensoTableOutcome secondary_outcome = DensoTableOutcome::kUnchanged;
    if (checksum_area_start == 0x1FF800)
    {
        const DensoTableSpec secondary{
            .table_address = fastecu::memory::FlashAddress{0x1FF8E8},
            .table_length = 24,
            .detect_disabled = false,
        };
        secondary_outcome = fastecu::checksum::internal::CorrectDensoTable(base, result.rom_data, secondary);
        if (const auto failure = fastecu::checksum::internal::FailureOf(secondary_outcome))
        {
            result.rom_data.assign(rom_view.begin(), rom_view.end());
            result.status = failure->status;
            result.message = failure->message;
            return result;
        }
    }

    if (primary_outcome == DensoTableOutcome::kCorrected || secondary_outcome == DensoTableOutcome::kCorrected)
    {
        result.status = ChecksumResult::Status::kCorrected;
        result.message = "Subaru Denso SH705x Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::kUnchanged;
    }
    return result;
}
