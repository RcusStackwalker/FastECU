#include "src/backend/calibration/session/rom_save.h"

#include <format>

namespace fastecu::calibration
{

RomSaveUseCase::RomSaveUseCase(IFileRepository& files, IEventSink& events) : files_(files), events_(events)
{
}

Status RomSaveUseCase::save(CalibrationSession& session, std::string_view path, bytes::ByteView image)
{
    const Status result = files_.write(path, image);
    if (!result.has_value())
    {
        events_.log(LogLevel::Error, std::format("Unable to open file {} for writing", path));
        events_.notice(std::format("Ecu calibration file: Unable to open file {} for writing", path));
        return result;
    }
    session.mark_saved(path);
    return {};
}

} // namespace fastecu::calibration
