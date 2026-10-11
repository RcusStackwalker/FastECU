#include "checksum_ecu_subaru_denso_sh7xxx.h"

#include "denso_checksum_table.h"

ChecksumResult ChecksumEcuSubaruDensoSH7xxx::CalculateChecksumResult(const fastecu::memory::MemoryView& rom,
                                                                     fastecu::memory::FlashAddress table_address,
                                                                     uint32_t table_length)
{
    ChecksumResult result;
    result.rom_data.assign(rom.Data().begin(), rom.Data().end());
    const fastecu::checksum::internal::DensoTableSpec spec{
        .table_address = table_address,
        .table_length = table_length,
    };

    using Outcome = fastecu::checksum::internal::DensoTableOutcome;
    switch (fastecu::checksum::internal::CorrectDensoTable(rom.Range().Start(), result.rom_data, spec))
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
