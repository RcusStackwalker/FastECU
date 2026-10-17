#include "denso_checksum_table.h"

#include <algorithm>
#include <optional>

namespace fastecu::checksum::internal
{
namespace
{
constexpr std::size_t kRecordLength = 12;
constexpr std::uint32_t kChecksumTarget = 0x5AA5A55A;

// The word at `offset` in `block`, which starts at ECU address `block_address`.
std::uint32_t WordAt(bytes::ByteView block, std::size_t offset, std::uint32_t block_address,
                     std::span<const DensoWordOverride> overrides)
{
    const std::uint32_t ecu_address = block_address + static_cast<std::uint32_t>(offset);
    const auto match = std::find_if(overrides.begin(), overrides.end(), [ecu_address](const DensoWordOverride& item)
                                    { return item.address == ecu_address; });
    return match == overrides.end() ? bytes::ReadU32Be(block, offset) : match->value;
}

// The sum of the words from ECU address `start` up to `end`, in `rom`, which
// starts at ECU address `base`; nullopt when one lies outside `rom`. A zero
// start or end sums nothing.
std::optional<std::uint32_t> SumBlock(bytes::ByteView rom, std::uint32_t base, std::uint32_t start, std::uint32_t end,
                                      std::span<const DensoWordOverride> overrides)
{
    if (start == 0 || end == 0)
    {
        return 0;
    }
    // Positions in `rom`. An end below `base` wraps past the end of `rom`
    // and fails the range check; a start below `base` alone wraps above the
    // end, which is a descending range and sums nothing, as legacy does for
    // any descending range.
    const std::uint32_t low = start - base;
    const std::uint32_t high = end - base;
    if (high > rom.size())
    {
        return std::nullopt;
    }
    if (low >= high)
    {
        return 0;
    }
    // Whole words from `low`, so the last may run past `high`; it must still
    // fit in `rom`. low < high <= rom.size(), so the subtraction cannot wrap.
    const std::size_t length = (std::size_t{high - low} + 3) & ~std::size_t{3};
    if (length > rom.size() - low)
    {
        return std::nullopt;
    }
    const bytes::ByteView block = rom.subspan(low, length);
    std::uint32_t sum = 0;
    for (std::size_t offset = 0; offset < block.size(); offset += 4)
    {
        sum += WordAt(block, offset, start, overrides);
    }
    return sum;
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
        const std::uint32_t start = bytes::ReadU32Be(rom, position);
        const std::uint32_t end = bytes::ReadU32Be(rom, position + 4);
        const std::uint32_t stored = bytes::ReadU32Be(rom, position + 8);

        if (record == 0 && spec.detect_disabled && start == 0 && end == 0 && stored == kChecksumTarget)
        {
            return DensoTableOutcome::kDisabled;
        }

        // A record already holding the target is left as it is.
        std::uint32_t sum = 0;
        if (stored != kChecksumTarget)
        {
            const std::optional<std::uint32_t> block_sum = SumBlock(rom, base.Value(), start, end, spec.overrides);
            if (!block_sum.has_value())
            {
                return DensoTableOutcome::kInvalidBlockRange;
            }
            sum = *block_sum;
        }
        const std::uint32_t expected = kChecksumTarget - sum;
        changed = changed || stored != expected;
        bytes::AppendU32Be(corrected, start);
        bytes::AppendU32Be(corrected, end);
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
