#include "denso_checksum_table.h"

#include <algorithm>
#include <optional>

namespace fastecu::checksum::internal
{
namespace
{
constexpr std::size_t kRecordLength = 12;
constexpr std::uint32_t kChecksumTarget = 0x5AA5A55A;

// The word at `position` in `rom`, which is ECU address `ecu_address`.
std::uint32_t WordAt(bytes::ByteView rom, std::uint32_t position, std::uint32_t ecu_address,
                     std::span<const DensoWordOverride> overrides)
{
    const auto match = std::find_if(overrides.begin(), overrides.end(), [ecu_address](const DensoWordOverride& item)
                                    { return item.address == ecu_address; });
    return match == overrides.end() ? bytes::ReadU32Be(rom, position) : match->value;
}
} // namespace

DensoTableOutcome CorrectDensoTable(memory::FlashAddress base, bytes::MutableByteView rom, const DensoTableSpec& spec)
{
    if (spec.table_length % kRecordLength != 0)
    {
        return DensoTableOutcome::kInvalidRecordLength;
    }
    const std::optional<memory::ByteCount> table_position = spec.table_address.DistanceFrom(base);
    if (!table_position.has_value() || table_position->Value() > rom.size() ||
        spec.table_length > rom.size() - table_position->Value())
    {
        return DensoTableOutcome::kInvalidTableRange;
    }

    bytes::Bytes corrected;
    corrected.reserve(spec.table_length);
    bool changed = false;

    for (std::size_t record = 0; record < spec.table_length; record += kRecordLength)
    {
        const std::size_t position = table_position->Value() + record;
        const std::uint32_t raw_low = bytes::ReadU32Be(rom, position);
        const std::uint32_t raw_high = bytes::ReadU32Be(rom, position + 4);
        const std::uint32_t stored = bytes::ReadU32Be(rom, position + 8);

        if (record == 0 && spec.detect_disabled && raw_low == 0 && raw_high == 0 && stored == kChecksumTarget)
        {
            return DensoTableOutcome::kDisabled;
        }

        // Positions in `rom`. An address below `base` wraps past the end of
        // `rom` and fails the range checks below.
        const std::uint32_t low = raw_low - base.Value();
        const std::uint32_t high = raw_high - base.Value();
        std::uint32_t sum = 0;
        if (raw_low != 0 && raw_high != 0 && stored != kChecksumTarget)
        {
            if (high > rom.size())
            {
                return DensoTableOutcome::kInvalidBlockRange;
            }
            for (std::uint32_t address = low; address < high; address += 4)
            {
                if (address > rom.size() || 4 > rom.size() - address)
                {
                    return DensoTableOutcome::kInvalidBlockRange;
                }
                sum += WordAt(rom, address, address + base.Value(), spec.overrides);
            }
        }
        const std::uint32_t expected = kChecksumTarget - sum;
        changed = changed || stored != expected;
        bytes::AppendU32Be(corrected, raw_low);
        bytes::AppendU32Be(corrected, raw_high);
        bytes::AppendU32Be(corrected, expected);
    }

    if (!changed)
    {
        return DensoTableOutcome::kUnchanged;
    }
    bytes::OverwriteAt(rom, table_position->Value(), corrected);
    return DensoTableOutcome::kCorrected;
}

} // namespace fastecu::checksum::internal
