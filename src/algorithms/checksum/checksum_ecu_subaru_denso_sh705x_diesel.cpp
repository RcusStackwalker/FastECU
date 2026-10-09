#include "checksum_ecu_subaru_denso_sh705x_diesel.h"

#include "denso_checksum_table.h"

#include <array>

ChecksumResult ChecksumEcuSubaruDensoSH705xDiesel::CalculateChecksumResult(bytes::ByteView rom_view,
                                                                           uint32_t checksum_area_start,
                                                                           uint32_t checksum_area_length)
{
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
        .table_offset = checksum_area_start,
        .table_length = checksum_area_length,
        .overrides = active_overrides,
    };
    const DensoTableOutcome primary_outcome = fastecu::checksum::internal::CorrectDensoTable(result.rom_data, primary);
    if (primary_outcome == DensoTableOutcome::kDisabled)
    {
        result.status = ChecksumResult::Status::kDisabled;
        // Disabled is a successful status, so the adapter propagates romData
        // back to FullRomData. Keep the original bytes here; the historical
        // empty return value could otherwise wipe the loaded ROM.
        result.message = "ROM has all checksums disabled";
        return result;
    }
    if (primary_outcome == DensoTableOutcome::kInvalidRecordLength)
    {
        result.status = ChecksumResult::Status::kParseError;
        result.message = "Checksum area length must be a multiple of 12 bytes";
        return result;
    }
    if (primary_outcome == DensoTableOutcome::kInvalidTableRange ||
        primary_outcome == DensoTableOutcome::kInvalidBlockRange)
    {
        result.status = ChecksumResult::Status::kInvalidSize;
        result.message = primary_outcome == DensoTableOutcome::kInvalidTableRange
                             ? "ROM is too small for the configured checksum area"
                             : "ROM is too small for a checksum block range";
        return result;
    }

    DensoTableOutcome secondary_outcome = DensoTableOutcome::kUnchanged;
    if (checksum_area_start == 0x1FF800)
    {
        const DensoTableSpec secondary{
            .table_offset = 0x1FF8E8,
            .table_length = 24,
            .detect_disabled = false,
        };
        secondary_outcome = fastecu::checksum::internal::CorrectDensoTable(result.rom_data, secondary);
        if (secondary_outcome == DensoTableOutcome::kInvalidTableRange ||
            secondary_outcome == DensoTableOutcome::kInvalidBlockRange)
        {
            result.rom_data.assign(rom_view.begin(), rom_view.end());
            result.status = ChecksumResult::Status::kInvalidSize;
            result.message = secondary_outcome == DensoTableOutcome::kInvalidTableRange
                                 ? "ROM is too small for the configured checksum area"
                                 : "ROM is too small for a checksum block range";
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
