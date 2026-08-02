#include "denso_checksum_table.h"

#include <algorithm>

namespace fastecu::checksum::internal
{
namespace
{
constexpr std::size_t kRecordLength = 12;
constexpr std::uint32_t kChecksumTarget = 0x5AA5A55A;

std::uint32_t wordAt(bytes::ByteView rom, std::uint32_t address,
                     std::span<const DensoWordOverride> overrides)
{
    const auto match = std::find_if(overrides.begin(), overrides.end(),
                                    [address](const DensoWordOverride& item)
                                    {
                                        return item.address == address;
                                    });
    return match == overrides.end() ? bytes::readU32Be(rom, address) : match->value;
}
} // namespace

DensoTableOutcome correctDensoTable(bytes::Bytes& rom, const DensoTableSpec& spec)
{
    if (spec.table_length % kRecordLength != 0)
    {
        return DensoTableOutcome::InvalidRecordLength;
    }
    if (spec.table_offset > rom.size() || spec.table_length > rom.size() - spec.table_offset)
    {
        return DensoTableOutcome::InvalidTableRange;
    }

    bytes::Bytes corrected;
    corrected.reserve(spec.table_length);
    bool changed = false;
    std::int32_t address_offset = spec.address_offset;

    for (std::size_t record = 0; record < spec.table_length; record += kRecordLength)
    {
        const std::size_t position = spec.table_offset + record;
        const std::uint32_t raw_low = bytes::readU32Be(rom, position);
        const std::uint32_t raw_high = bytes::readU32Be(rom, position + 4);
        const std::uint32_t stored = bytes::readU32Be(rom, position + 8);
        if (raw_low == 0 && raw_high == 0)
        {
            address_offset = 0;
        }
        const std::uint32_t low = raw_low + static_cast<std::uint32_t>(address_offset);
        const std::uint32_t high = raw_high + static_cast<std::uint32_t>(address_offset);

        if (record == 0 && spec.detect_disabled && low == 0 && high == 0 && stored == kChecksumTarget)
        {
            return DensoTableOutcome::Disabled;
        }

        std::uint32_t sum = 0;
        if (low != 0 && high != 0 && stored != kChecksumTarget)
        {
            if (low > high || high > rom.size() || (high - low) % 4 != 0)
            {
                return DensoTableOutcome::InvalidBlockRange;
            }
            for (std::uint32_t address = low; address < high; address += 4)
            {
                if (address > rom.size() || 4 > rom.size() - address)
                {
                    return DensoTableOutcome::InvalidBlockRange;
                }
                sum += wordAt(rom, address, spec.overrides);
            }
        }
        const std::uint32_t expected = kChecksumTarget - sum;
        changed = changed || stored != expected;
        bytes::appendU32Be(corrected, raw_low);
        bytes::appendU32Be(corrected, raw_high);
        bytes::appendU32Be(corrected, expected);
    }

    if (!changed)
    {
        return DensoTableOutcome::Unchanged;
    }
    bytes::overwriteAt(rom, spec.table_offset, corrected);
    return DensoTableOutcome::Corrected;
}

} // namespace fastecu::checksum::internal
