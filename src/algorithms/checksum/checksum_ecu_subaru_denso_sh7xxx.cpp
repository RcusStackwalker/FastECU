#include "checksum_ecu_subaru_denso_sh7xxx.h"

#include "denso_checksum_table.h"

ChecksumResult ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::ByteView romData,
                                                                       uint32_t checksum_area_start,
                                                                       uint32_t checksum_area_length, int32_t offset)
{
    ChecksumResult result;
    result.rom_data.assign(romData.begin(), romData.end());
    const fastecu::checksum::internal::DensoTableSpec spec{
        .table_offset = checksum_area_start,
        .table_length = checksum_area_length,
        .address_offset = offset,
    };

    using Outcome = fastecu::checksum::internal::DensoTableOutcome;
    switch (fastecu::checksum::internal::correctDensoTable(result.rom_data, spec))
    {
    case Outcome::kUnchanged:
        result.status = ChecksumResult::Status::kUnchanged;
        result.message = "Checksums OK";
        break;
    case Outcome::kCorrected:
        result.status = ChecksumResult::Status::kCorrected;
        result.message = "Checksums corrected";
        break;
    case Outcome::kDisabled:
        result.status = ChecksumResult::Status::kDisabled;
        result.message = "ROM has all checksums disabled";
        break;
    case Outcome::kInvalidRecordLength:
        result.status = ChecksumResult::Status::kParseError;
        result.message = "Checksum area length must be a multiple of 12 bytes";
        break;
    case Outcome::kInvalidTableRange:
        result.status = ChecksumResult::Status::kInvalidSize;
        result.message = "ROM is too small for the configured checksum area";
        break;
    case Outcome::kInvalidBlockRange:
        result.status = ChecksumResult::Status::kInvalidSize;
        result.message = "ROM is too small for a checksum block range";
        break;
    }
    return result;
}
