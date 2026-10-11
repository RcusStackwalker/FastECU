#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "src/algorithms/protocol/bytes.h"

namespace fastecu::calibration
{
// ROM bytes as a definition addresses them: the byte at definition address
// `start + i` is data[i]. Decoders and edits read through At(), so they run on
// a window rendered from a memory image as well as on a whole ROM; a plain
// byte buffer converts to a window at definition address 0.
class DefinitionBytes
{
  public:
    DefinitionBytes(bytes::ByteView data, std::uint64_t start = 0) : data_(data), start_(start)
    {
    }
    DefinitionBytes(const bytes::Bytes& data, std::uint64_t start = 0) : data_(data), start_(start)
    {
    }

    // The `size` bytes from definition address `address`; nullopt when any of
    // them lies outside the window.
    [[nodiscard]] std::optional<bytes::ByteView> At(std::uint64_t address, std::uint64_t size) const
    {
        if (address < start_)
        {
            return std::nullopt;
        }
        const std::uint64_t offset = address - start_;
        if (offset > data_.size() || size > data_.size() - offset)
        {
            return std::nullopt;
        }
        return data_.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
    }

  private:
    bytes::ByteView data_;
    std::uint64_t start_;
};
} // namespace fastecu::calibration
