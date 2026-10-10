#pragma once

#include <string>

namespace fastecu::memory
{
enum class MemoryErrorKind
{
    kInvalidLayout,    // A memory map's blocks do not describe one ROM file layout.
    kFileSizeMismatch, // A ROM file is not the size its memory map places.
    kUnmapped,         // An address belongs to no memory block.
    kFillBlock,        // A write reaches a fill block, which has no ROM file bytes.
    kNotWritable,      // A write reaches a memory block that is not writable.
};

struct MemoryError
{
    MemoryErrorKind kind{MemoryErrorKind::kInvalidLayout};
    std::string detail;
};
} // namespace fastecu::memory
