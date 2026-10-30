#include "src/backend/calibration/session/calibration_session.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/calibration/map_placement.h"

namespace fastecu::calibration
{
namespace
{
// One past the highest definition address any block holds: what Read and
// CheckWrite accept.
std::uint64_t DefinitionSize(const memory::MemoryMap& map)
{
    const std::uint32_t base = map.DefinitionBase().Value();
    std::uint32_t end = base;
    for (const memory::MemoryBlock& block : map.Blocks())
    {
        end = std::max(end, block.range.End().Value());
    }
    return end - base;
}

// The ECU addresses [start, end), which lie inside the blocks' span: every
// block's share rendered from `image`, 0x00 where no block holds an address.
bytes::Bytes RenderWindow(const memory::MemoryImage& image, std::uint32_t start, std::uint32_t end)
{
    bytes::Bytes window(end - start, 0x00);
    for (const memory::MemoryBlock& block : image.Map().Blocks())
    {
        const std::uint32_t from = std::max(block.range.Start().Value(), start);
        const std::uint32_t to = std::min(block.range.End().Value(), end);
        if (from >= to)
        {
            continue;
        }
        // Non-empty and inside one block, so the range exists and renders.
        const auto range =
            memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{from}, memory::ByteCount{to - from});
        if (const auto rendered = image.Render(*range); rendered.has_value())
        {
            std::ranges::copy(rendered->Data(), window.begin() + static_cast<std::ptrdiff_t>(from - start));
        }
    }
    return window;
}
} // namespace

CalibrationSession::CalibrationSession(SessionId id, SessionContents contents)
    : id_(id), source_(std::move(contents.source)), image_(std::move(contents.image)),
      definition_(std::move(contents.definition)), protocol_(std::move(contents.protocol))
{
}

SessionId CalibrationSession::Id() const
{
    return id_;
}

const RomSource& CalibrationSession::Source() const
{
    return source_;
}

bytes::ByteView CalibrationSession::File() const
{
    return image_.File();
}

const ResolvedDefinition *CalibrationSession::Definition() const
{
    return definition_.has_value() ? &*definition_ : nullptr;
}

const RomProtocolInfo& CalibrationSession::Protocol() const
{
    return protocol_;
}

void CalibrationSession::SetProtocol(RomProtocolInfo protocol)
{
    protocol_ = std::move(protocol);
}

bool CalibrationSession::Dirty() const
{
    return dirty_;
}

void CalibrationSession::MarkSaved(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    // Copy both views before changing source: path may refer to source().path.
    std::string saved_path{path};
    std::string saved_name = name.empty() ? std::string{"default.bin"} : std::string{name};
    source_.path = std::move(saved_path);
    source_.display_name = std::move(saved_name);
    dirty_ = false;
}

Result<DecodedMap> CalibrationSession::DecodeMap(std::size_t map_index) const
{
    if (!definition_.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "calibration session has no definition");
    }
    const definition::RomDefinition& rom_definition = definition_->definition;
    if (map_index >= rom_definition.maps.size())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("map index {} is past the definition's {} maps", map_index,
                                                           rom_definition.maps.size()));
    }
    const definition::CalibrationMap& map = rom_definition.maps[map_index];
    return CheckMapPlacement(rom_definition, map, image_.Map())
        .and_then(
            [&](const MapExtent& extent)
            {
                return Read(extent.start, static_cast<std::size_t>(extent.end - extent.start))
                    .and_then(
                        [&](const bytes::Bytes& window)
                        { return DecodeCalibrationMap(rom_definition, map, DefinitionBytes(window, extent.start)); });
            });
}

Result<bytes::Bytes> CalibrationSession::Read(std::uint64_t offset, std::size_t size) const
{
    const std::uint64_t view_size = DefinitionSize(image_.Map());
    // Written as two comparisons so a huge offset cannot wrap the sum.
    if (offset > view_size || size > view_size - offset)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("read of {} bytes at 0x{:x} is outside the {}-byte image", size, offset, view_size));
    }
    // Inside the view, so both ends are ECU addresses no larger than the
    // highest block's end.
    const std::uint32_t start = image_.Map().DefinitionBase().Value() + static_cast<std::uint32_t>(offset);
    return RenderWindow(image_, start, start + static_cast<std::uint32_t>(size));
}

Status CalibrationSession::CheckWrite(std::uint64_t offset, std::size_t size) const
{
    const std::uint64_t view_size = DefinitionSize(image_.Map());
    // Written as two comparisons so a huge offset cannot wrap the sum.
    if (offset > view_size || size > view_size - offset)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("write of {} bytes at 0x{:x} is outside the {}-byte image", size, offset, view_size));
    }
    if (size == 0)
    {
        return {};
    }
    // Inside the view, so the offset fits in 32 bits and the address exists.
    const memory::FlashAddress start =
        *image_.Map().ToFlashAddress(memory::DefinitionAddress{static_cast<std::uint32_t>(offset)});
    if (const auto checked = image_.CheckWrite(start, memory::ByteCount{static_cast<std::uint32_t>(size)});
        !checked.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("write of {} bytes at 0x{:x}: {}", size, offset, checked.error().detail));
    }
    return {};
}

Status CalibrationSession::WriteBytes(std::uint64_t offset, bytes::ByteView data)
{
    if (const Status checked = CheckWrite(offset, data.size()); !checked.has_value())
    {
        return checked;
    }
    if (!data.empty())
    {
        // CheckWrite passed, so the offset fits in 32 bits and the address exists.
        const memory::FlashAddress start =
            *image_.Map().ToFlashAddress(memory::DefinitionAddress{static_cast<std::uint32_t>(offset)});
        if (const auto written = image_.Write(start, data); !written.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, written.error().detail);
        }
    }
    dirty_ = true;
    return {};
}

} // namespace fastecu::calibration
