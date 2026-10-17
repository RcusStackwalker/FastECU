#pragma once

#include "checksum_result.h"
#include "src/algorithms/memory/address.h"
#include "src/algorithms/protocol/bytes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace fastecu::checksum::internal
{

struct DensoWordOverride
{
    std::uint32_t address; // ECU address
    std::uint32_t value;
};

struct DensoTableSpec
{
    memory::FlashAddress table_address;
    std::uint32_t table_length = 0;
    std::span<const DensoWordOverride> overrides;
    bool detect_disabled = true;
};

enum class DensoTableOutcome
{
    kUnchanged,
    kCorrected,
    kDisabled,
    kInvalidTableRange,
    kInvalidBlockRange,
    kReversedBlockRange,
    kInvalidRecordLength,
};

// `rom` holds the bytes from ECU address `base` on. The table's records hold
// ECU addresses; a record with a zero start or end address sums nothing.
DensoTableOutcome CorrectDensoTable(memory::FlashAddress base, bytes::MutableByteView rom, const DensoTableSpec& spec);

struct DensoTableFailure
{
    ChecksumResult::Status status;
    const char *message;
};

// What a family reports when CorrectDensoTable gives `outcome`; nullopt for
// the outcomes that are not failures.
std::optional<DensoTableFailure> FailureOf(DensoTableOutcome outcome);

} // namespace fastecu::checksum::internal
