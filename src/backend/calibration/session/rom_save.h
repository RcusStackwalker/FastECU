#pragma once

#include <string_view>

#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/file_repository.h"

namespace fastecu::calibration
{

// Persists the caller's operation image, which may include checksum corrections
// without replacing the editable session bytes. Marks the session saved only
// after the repository confirms the write succeeded.
class RomSaveUseCase
{
  public:
    RomSaveUseCase(IFileRepository& files, IEventSink& events);
    Status save(CalibrationSession& session, std::string_view path, bytes::ByteView image);

  private:
    IFileRepository& files_;
    IEventSink& events_;
};

} // namespace fastecu::calibration
