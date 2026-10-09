#include "src/backend/calibration/session/calibration_workspace.h"

#include <algorithm>
#include <format>
#include <utility>

namespace fastecu::calibration
{

CalibrationWorkspace::CalibrationWorkspace(RomOpenUseCase& opener) : opener_(opener)
{
}

Result<OpenedSession> CalibrationWorkspace::OpenFile(std::string_view path)
{
    Result<RomOpenOutcome> outcome = opener_.OpenFile(path);
    if (!outcome.has_value())
    {
        return std::unexpected(outcome.error());
    }
    return Insert(std::move(*outcome));
}

Result<OpenedSession> CalibrationWorkspace::AdoptReadImage(ReadImage image)
{
    Result<RomOpenOutcome> outcome = opener_.AdoptReadImage(std::move(image));
    if (!outcome.has_value())
    {
        return std::unexpected(outcome.error());
    }
    return Insert(std::move(*outcome));
}

Status CalibrationWorkspace::Close(SessionId id)
{
    const auto found = std::ranges::find_if(sessions_, [id](const auto& session) { return session->Id() == id; });
    if (found == sessions_.end())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("no open calibration session {}", static_cast<std::uint64_t>(id)));
    }
    sessions_.erase(found);
    return {};
}

CalibrationSession *CalibrationWorkspace::Find(SessionId id)
{
    const auto found = std::ranges::find_if(sessions_, [id](const auto& session) { return session->Id() == id; });
    return found == sessions_.end() ? nullptr : found->get();
}

const CalibrationSession *CalibrationWorkspace::Find(SessionId id) const
{
    const auto found = std::ranges::find_if(sessions_, [id](const auto& session) { return session->Id() == id; });
    return found == sessions_.end() ? nullptr : found->get();
}

std::vector<SessionId> CalibrationWorkspace::Ids() const
{
    std::vector<SessionId> result;
    result.reserve(sessions_.size());
    for (const auto& session : sessions_)
    {
        result.push_back(session->Id());
    }
    return result;
}

OpenedSession CalibrationWorkspace::Insert(RomOpenOutcome outcome)
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
