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
    const Outcome outcome = fastecu::checksum::internal::CorrectDensoTable(rom.Range().Start(), result.rom_data, spec);
    if (const auto failure = fastecu::checksum::internal::FailureOf(outcome))
    {
        result.status = failure->status;
        result.message = failure->message;
        return result;
    }
    switch (outcome)
    {
    case Outcome::kCorrected:
        result.status = ChecksumResult::Status::kCorrected;
        result.message = "Checksums corrected";
        break;
    case Outcome::kDisabled:
        result.status = ChecksumResult::Status::kDisabled;
        result.message = "ROM has all checksums disabled";
        break;
    default:
        result.status = ChecksumResult::Status::kUnchanged;
        result.message = "Checksums OK";
        break;
    }
    return result;
}
