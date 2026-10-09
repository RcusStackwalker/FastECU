#include "src/backend/calibration/session/calibration_workspace.h"

#include <algorithm>
#include <format>
#include <utility>

namespace fastecu::calibration
{

CalibrationWorkspace::CalibrationWorkspace(RomOpenUseCase& opener) : opener_(opener)
{
}

Result<OpenedSession> CalibrationWorkspace::open_file(std::string_view path)
{
    Result<RomOpenOutcome> outcome = opener_.open_file(path);
    if (!outcome.has_value())
    {
        return std::unexpected(outcome.error());
    }
    return insert(std::move(*outcome));
}

Result<OpenedSession> CalibrationWorkspace::adopt_read_image(ReadImage image)
{
    Result<RomOpenOutcome> outcome = opener_.adopt_read_image(std::move(image));
    if (!outcome.has_value())
    {
        return std::unexpected(outcome.error());
    }
    return insert(std::move(*outcome));
}

Status CalibrationWorkspace::close(SessionId id)
{
    const auto found = std::ranges::find_if(sessions_, [id](const auto& session) { return session->id() == id; });
    if (found == sessions_.end())
    {
        return fail(ErrorKind::kInvalidConfig,
                    std::format("no open calibration session {}", static_cast<std::uint64_t>(id)));
    }
    sessions_.erase(found);
    return {};
}

CalibrationSession *CalibrationWorkspace::find(SessionId id)
{
    const auto found = std::ranges::find_if(sessions_, [id](const auto& session) { return session->id() == id; });
    return found == sessions_.end() ? nullptr : found->get();
}

const CalibrationSession *CalibrationWorkspace::find(SessionId id) const
{
    const auto found = std::ranges::find_if(sessions_, [id](const auto& session) { return session->id() == id; });
    return found == sessions_.end() ? nullptr : found->get();
}

std::vector<SessionId> CalibrationWorkspace::ids() const
{
    std::vector<SessionId> result;
    result.reserve(sessions_.size());
    for (const auto& session : sessions_)
    {
        result.push_back(session->id());
    }
    return result;
}

OpenedSession CalibrationWorkspace::insert(RomOpenOutcome outcome)
{
    const SessionId id{next_id_++};
    sessions_.push_back(std::make_unique<CalibrationSession>(id, std::move(outcome.contents)));
    return OpenedSession{
        .id = id,
        .vehicle_selected = outcome.vehicle_selected,
        .size_rejected = outcome.size_rejected,
    };
}

} // namespace fastecu::calibration
