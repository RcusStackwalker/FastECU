#include "src/backend/calibration/session/calibration_session.h"

#include <algorithm>
#include <format>
#include <utility>

namespace fastecu::calibration
{

CalibrationSession::CalibrationSession(SessionId id, SessionContents contents) : id_(id), contents_(std::move(contents))
{
}

SessionId CalibrationSession::id() const
{
    return id_;
}

const RomSource& CalibrationSession::source() const
{
    return contents_.source;
}

bytes::ByteView CalibrationSession::rom() const
{
    return contents_.rom;
}

const ResolvedDefinition *CalibrationSession::definition() const
{
    return contents_.definition.has_value() ? &*contents_.definition : nullptr;
}

const RomProtocolInfo& CalibrationSession::protocol() const
{
    return contents_.protocol;
}

void CalibrationSession::set_protocol(RomProtocolInfo protocol)
{
    contents_.protocol = std::move(protocol);
}

bool CalibrationSession::dirty() const
{
    return dirty_;
}

Result<MapCellValues> CalibrationSession::decode_map(std::size_t map_index) const
{
    if (!contents_.definition.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "calibration session has no definition");
    }
    const definition::RomDefinition& rom_definition = contents_.definition->definition;
    if (map_index >= rom_definition.maps.size())
    {
        return fail(ErrorKind::InvalidConfig, std::format("map index {} is past the definition's {} maps", map_index,
                                                          rom_definition.maps.size()));
    }
    return compute_one_map_cell_values(rom_definition, rom_definition.maps[map_index], contents_.rom,
                                       kCellFloatPrecision);
}

Status CalibrationSession::write_bytes(std::uint64_t offset, bytes::ByteView data)
{
    const std::uint64_t size = contents_.rom.size();
    // Written as two comparisons so a huge offset cannot wrap the sum.
    if (offset > size || data.size() > size - offset)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("write of {} bytes at 0x{:x} is outside the {}-byte image", data.size(), offset, size));
    }
    std::ranges::copy(data, contents_.rom.begin() + static_cast<std::ptrdiff_t>(offset));
    dirty_ = true;
    return {};
}

} // namespace fastecu::calibration
