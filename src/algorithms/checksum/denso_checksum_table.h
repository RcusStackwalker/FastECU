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
    std::span<const DensoWordOverride> overrides{};
    bool detect_disabled = true;
};

enum class DensoTableOutcome
{
    kUnchanged,
    kCorrected,
    kDisabled,
    kInvalidTableRange,
    kInvalidBlockRange,
    kMalformedBlockRange,
    kInvalidRecordLength,
};

// `rom` holds the bytes from ECU address `base` on. Each record holds a
// block's start and end ECU addresses, the end being the block's last byte,
// and the value that brings the block's word sum to the target.
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
