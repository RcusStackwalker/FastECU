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
// Index i holds the byte at definition base + i, up to the end of the highest
// block; bytes no block holds stay 0x00.
bytes::Bytes DefinitionView(const memory::MemoryImage& image)
{
    const memory::MemoryMap& map = image.Map();
    const std::uint32_t base = map.DefinitionBase().Value();
    std::uint32_t end = base;
    for (const memory::MemoryBlock& block : map.Blocks())
    {
        end = std::max(end, block.range.End().Value());
    }
    bytes::Bytes view(end - base, 0x00);
    for (const memory::MemoryBlock& block : map.Blocks())
    {
        if (block.range.End().Value() <= base)
        {
            continue;
        }
        const std::uint32_t start = std::max(block.range.Start().Value(), base);
        const auto range = memory::AddressRange<memory::FlashSpace>::Make(
            memory::FlashAddress{start}, memory::ByteCount{block.range.End().Value() - start});
        if (!range.has_value())
        {
            continue;
        }
        if (const auto rendered = image.Render(*range); rendered.has_value())
        {
            std::ranges::copy(rendered->Data(), view.begin() + static_cast<std::ptrdiff_t>(start - base));
        }
    }
    return view;
}
} // namespace

CalibrationSession::CalibrationSession(SessionId id, SessionContents contents)
    : id_(id), source_(std::move(contents.source)), image_(std::move(contents.image)),
      definition_view_(DefinitionView(image_)), definition_(std::move(contents.definition)),
      protocol_(std::move(contents.protocol))
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

bytes::ByteView CalibrationSession::Rom() const
{
    return definition_view_;
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
    if (const Status placed = CheckMapPlacement(rom_definition, map, image_.Map()); !placed.has_value())
    {
        return std::unexpected(placed.error());
    }
    return DecodeCalibrationMap(rom_definition, map, definition_view_);
}

Status CalibrationSession::CheckWrite(std::uint64_t offset, std::size_t size) const
{
    const std::uint64_t view_size = definition_view_.size();
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
        std::ranges::copy(data, definition_view_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    dirty_ = true;
    return {};
}

} // namespace fastecu::calibration
