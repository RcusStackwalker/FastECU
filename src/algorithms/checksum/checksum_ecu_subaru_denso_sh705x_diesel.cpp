#include "checksum_ecu_subaru_denso_sh705x_diesel.h"

#include "denso_checksum_table.h"

#include <array>

ChecksumResult ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum_result(
    bytes::ByteView romView, uint32_t checksum_area_start,
    uint32_t checksum_area_length)
{
    using fastecu::checksum::internal::DensoTableOutcome;
    using fastecu::checksum::internal::DensoTableSpec;
    using fastecu::checksum::internal::DensoWordOverride;

    ChecksumResult result;
    result.romData.assign(romView.begin(), romView.end());
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
    const DensoTableOutcome primary_outcome =
        fastecu::checksum::internal::correctDensoTable(result.romData, primary);
    if (primary_outcome == DensoTableOutcome::Disabled)
    {
        result.status = ChecksumResult::Status::Disabled;
        result.romData.clear();
        result.message = "ROM has all checksums disabled";
        return result;
    }
    if (primary_outcome == DensoTableOutcome::InvalidRecordLength)
    {
        result.status = ChecksumResult::Status::ParseError;
        result.message = "Checksum area length must be a multiple of 12 bytes";
        return result;
    }
    if (primary_outcome == DensoTableOutcome::InvalidTableRange ||
        primary_outcome == DensoTableOutcome::InvalidBlockRange)
    {
        result.romData.assign(romView.begin(), romView.end());
        result.status = ChecksumResult::Status::InvalidSize;
        result.message = primary_outcome == DensoTableOutcome::InvalidTableRange
                             ? "ROM is too small for the configured checksum area"
                             : "ROM is too small for a checksum block range";
        return result;
    }

    DensoTableOutcome secondary_outcome = DensoTableOutcome::Unchanged;
    if (checksum_area_start == 0x1FF800)
    {
        const DensoTableSpec secondary{
            .table_offset = 0x1FF8E8,
            .table_length = 24,
            .detect_disabled = false,
        };
        secondary_outcome = fastecu::checksum::internal::correctDensoTable(result.romData, secondary);
        if (secondary_outcome == DensoTableOutcome::InvalidTableRange ||
            secondary_outcome == DensoTableOutcome::InvalidBlockRange)
        {
            result.romData.assign(romView.begin(), romView.end());
            result.status = ChecksumResult::Status::InvalidSize;
            result.message = secondary_outcome == DensoTableOutcome::InvalidTableRange
                                 ? "ROM is too small for the configured checksum area"
                                 : "ROM is too small for a checksum block range";
            return result;
        }
    }

    if (primary_outcome == DensoTableOutcome::Corrected ||
        secondary_outcome == DensoTableOutcome::Corrected)
    {
        result.status = ChecksumResult::Status::Corrected;
        result.message = "Subaru Denso SH705x Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::Unchanged;
    }
    return result;
}
