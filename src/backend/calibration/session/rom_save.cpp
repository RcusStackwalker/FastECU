#include "src/backend/calibration/session/rom_save.h"

#include <format>

namespace fastecu::calibration
{

RomSaveUseCase::RomSaveUseCase(IFileRepository& files, IEventSink& events) : files_(files), events_(events)
{
}

Status RomSaveUseCase::Save(CalibrationSession& session, std::string_view path, bytes::ByteView image)
{
    const Status result = files_.Write(path, image);
    if (!result.has_value())
    {
        events_.Log(LogLevel::kError, std::format("Unable to open file {} for writing", path));
        events_.Notice(std::format("Ecu calibration file: Unable to open file {} for writing", path));
        return result;
    }
    session.MarkSaved(path);
    return {};
}

} // namespace fastecu::calibration
