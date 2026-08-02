#include "checksum_ecu_subaru_denso_sh7xxx.h"

#include "denso_checksum_table.h"

ChecksumResult ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(
    bytes::ByteView romData, uint32_t checksum_area_start,
    uint32_t checksum_area_length, int32_t offset)
{
    ChecksumResult result;
    result.romData.assign(romData.begin(), romData.end());
    const fastecu::checksum::internal::DensoTableSpec spec{
        .table_offset = checksum_area_start,
        .table_length = checksum_area_length,
        .address_offset = offset,
    };

    using Outcome = fastecu::checksum::internal::DensoTableOutcome;
    switch (fastecu::checksum::internal::correctDensoTable(result.romData, spec))
    {
    case Outcome::Unchanged:
        result.status = ChecksumResult::Status::Unchanged;
        result.message = "Checksums OK";
        break;
    case Outcome::Corrected:
        result.status = ChecksumResult::Status::Corrected;
        result.message = "Checksums corrected";
        break;
    case Outcome::Disabled:
        result.status = ChecksumResult::Status::Disabled;
        result.message = "ROM has all checksums disabled";
        break;
    case Outcome::InvalidRecordLength:
        result.status = ChecksumResult::Status::ParseError;
        result.message = "Checksum area length must be a multiple of 12 bytes";
        break;
    case Outcome::InvalidTableRange:
        result.status = ChecksumResult::Status::InvalidSize;
        result.message = "ROM is too small for the configured checksum area";
        break;
    case Outcome::InvalidBlockRange:
        result.status = ChecksumResult::Status::InvalidSize;
        result.message = "ROM is too small for a checksum block range";
        break;
    }
    return result;
}
