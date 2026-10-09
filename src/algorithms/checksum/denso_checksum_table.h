#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace fastecu::checksum::internal
{

struct DensoWordOverride
{
    std::uint32_t address;
    std::uint32_t value;
};

struct DensoTableSpec
{
    std::size_t table_offset = 0;
    std::size_t table_length = 0;
    std::int32_t address_offset = 0;
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
    kInvalidRecordLength,
};

DensoTableOutcome correctDensoTable(bytes::MutableByteView rom, const DensoTableSpec& spec);

} // namespace fastecu::checksum::internal
