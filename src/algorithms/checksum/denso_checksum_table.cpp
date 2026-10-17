#include "denso_checksum_table.h"

#include <algorithm>
#include <expected>
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
// starts at ECU address `base`. A zero start or end sums nothing.
std::expected<std::uint32_t, DensoTableOutcome> SumBlock(bytes::ByteView rom, std::uint32_t base, std::uint32_t start,
                                                         std::uint32_t end,
                                                         std::span<const DensoWordOverride> overrides)
{
    if (start == 0 || end == 0)
    {
        return 0;
    }
    // Legacy summed nothing here, but only because its loop never ran. The
    // ECU's own check (SH7055 and SH7058 petrol firmware) fails such a
    // record before summing it.
    if (end < start)
    {
        return std::unexpected(DensoTableOutcome::kReversedBlockRange);
    }
    if (start < base || end - base > rom.size())
    {
        return std::unexpected(DensoTableOutcome::kInvalidBlockRange);
    }
    // Positions in `rom`: low <= high <= rom.size().
    const std::size_t low = start - base;
    const std::size_t high = end - base;
    // Whole words from `low`, so the last may run past `high`; it must still
    // fit in `rom`.
    const std::size_t length = (high - low + 3) & ~std::size_t{3};
    if (length > rom.size() - low)
    {
        return std::unexpected(DensoTableOutcome::kInvalidBlockRange);
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
            const std::expected<std::uint32_t, DensoTableOutcome> block_sum =
                SumBlock(rom, base.Value(), start, end, spec.overrides);
            if (!block_sum.has_value())
            {
                return block_sum.error();
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

std::optional<DensoTableFailure> FailureOf(DensoTableOutcome outcome)
{
    switch (outcome)
    {
    case DensoTableOutcome::kUnchanged:
    case DensoTableOutcome::kCorrected:
    case DensoTableOutcome::kDisabled:
        return std::nullopt;
    case DensoTableOutcome::kInvalidTableRange:
        return DensoTableFailure{ChecksumResult::Status::kInvalidSize,
                                 "ROM is too small for the configured checksum area"};
    case DensoTableOutcome::kInvalidBlockRange:
        return DensoTableFailure{ChecksumResult::Status::kInvalidSize, "ROM is too small for a checksum block range"};
    case DensoTableOutcome::kReversedBlockRange:
        return DensoTableFailure{ChecksumResult::Status::kParseError, "A checksum block range ends before it starts"};
    case DensoTableOutcome::kInvalidRecordLength:
        return DensoTableFailure{ChecksumResult::Status::kParseError,
                                 "Checksum area length must be a multiple of 12 bytes"};
    }
    return std::nullopt;
}

} // namespace fastecu::checksum::internal
