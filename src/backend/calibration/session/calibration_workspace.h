#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/rom_open.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

struct OpenedSession
{
    SessionId id{};
    bool vehicle_selected{false};
};

// The open calibrations, replacing MainWindow's fixed array of 100 raw
// pointers. A session enters only after its open or adoption succeeded, so a
// failed or cancelled read never occupies an entry. Owned by the desktop
// composition, which outlives every window that holds a SessionId.
class CalibrationWorkspace
{
  public:
    explicit CalibrationWorkspace(RomOpenUseCase& opener);

    CalibrationWorkspace(const CalibrationWorkspace&) = delete;
    CalibrationWorkspace& operator=(const CalibrationWorkspace&) = delete;

    Result<OpenedSession> OpenFile(std::string_view path);
    Result<OpenedSession> AdoptReadImage(ReadImage image);
    // InvalidConfig for an ID that is not open.
    Status Close(SessionId id);

    // nullptr for an ID that is not open -- an expected outcome for a UI
    // element outliving its session. Pointers stay valid until that session
    // is closed.
    CalibrationSession *Find(SessionId id);
    const CalibrationSession *Find(SessionId id) const;
    std::vector<SessionId> Ids() const;

  private:
    OpenedSession Insert(RomOpenOutcome outcome);

    RomOpenUseCase& opener_;
    std::uint64_t next_id_{1};
    std::vector<std::unique_ptr<CalibrationSession>> sessions_;
};

} // namespace fastecu::calibration
