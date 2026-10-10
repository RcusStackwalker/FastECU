#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>

namespace fastecu::memory
{
// A number of bytes. Distinct from element counts and from positions.
class ByteCount
{
  public:
    constexpr ByteCount() = default;
    explicit constexpr ByteCount(std::uint32_t value) : value_(value)
    {
    }

    [[nodiscard]] constexpr std::uint32_t Value() const
    {
        return value_;
    }

    constexpr auto operator<=>(const ByteCount&) const = default;

  private:
    std::uint32_t value_{0};
};

// A 32-bit position tagged with what it is a position in. Positions with
// different tags never mix: there is no conversion, comparison or arithmetic
// between them, and none with plain integers. Arithmetic fails rather than
// wrapping.
template <typename Tag> class TaggedPosition
{
  public:
    constexpr TaggedPosition() = default;
    explicit constexpr TaggedPosition(std::uint32_t value) : value_(value)
    {
    }

    [[nodiscard]] constexpr std::uint32_t Value() const
    {
        return value_;
    }

    // The position `count` bytes on, or nullopt past the end of the 32-bit space.
    [[nodiscard]] constexpr std::optional<TaggedPosition> Advance(ByteCount count) const
    {
        if (count.Value() > std::numeric_limits<std::uint32_t>::max() - value_)
        {
            return std::nullopt;
        }
        return TaggedPosition(value_ + count.Value());
    }

    // The bytes from `base` up to this position, or nullopt when `base` lies beyond it.
    [[nodiscard]] constexpr std::optional<ByteCount> DistanceFrom(TaggedPosition base) const
    {
        if (base.value_ > value_)
        {
            return std::nullopt;
        }
        return ByteCount(value_ - base.value_);
    }

    constexpr auto operator<=>(const TaggedPosition&) const = default;

  private:
    std::uint32_t value_{0};
};

// The address space of the ECU's program flash and whatever its CPU maps
// beside it (the MC68HC16Y5 RAM between two flash ranges, for example).
struct FlashSpace
{
};

// A location in one ECU address space, as the ECU's CPU sees it.
template <typename Space> using Address = TaggedPosition<Space>;
using FlashAddress = Address<FlashSpace>;

struct FileTag
{
};

// A byte position within a ROM file. Meaningful only when loading and saving.
using FileOffset = TaggedPosition<FileTag>;

// A non-empty half-open range [start, start + size) in one address space. Its
// end is always representable, so no range includes address 0xFFFFFFFF.
template <typename Space> class AddressRange
{
  public:
    // nullopt for an empty range or one whose end is past the 32-bit space.
    [[nodiscard]] static constexpr std::optional<AddressRange> Make(Address<Space> start, ByteCount size)
    {
        if (size.Value() == 0)
        {
            return std::nullopt;
        }
        const std::optional<Address<Space>> end = start.Advance(size);
        if (!end.has_value())
        {
            return std::nullopt;
        }
        return AddressRange(start, size, *end);
    }

    [[nodiscard]] constexpr Address<Space> Start() const
    {
        return start_;
    }
    // The first address after the range.
    [[nodiscard]] constexpr Address<Space> End() const
    {
        return end_;
    }
    [[nodiscard]] constexpr ByteCount Size() const
    {
        return size_;
    }

    [[nodiscard]] constexpr bool Contains(Address<Space> address) const
    {
        return start_ <= address && address < end_;
    }
    [[nodiscard]] constexpr bool Contains(const AddressRange& other) const
    {
        return start_ <= other.start_ && other.end_ <= end_;
    }
    [[nodiscard]] constexpr bool Overlaps(const AddressRange& other) const
    {
        return start_ < other.end_ && other.start_ < end_;
    }

    constexpr bool operator==(const AddressRange&) const = default;

  private:
    constexpr AddressRange(Address<Space> start, ByteCount size, Address<Space> end)
        : start_(start), size_(size), end_(end)
    {
    }

    Address<Space> start_;
    ByteCount size_;
    Address<Space> end_;
};
} // namespace fastecu::memory
