#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <QString>

#include "src/ui/desktop/calibration/session_key.h"
#include "src/backend/calibration/numeric_map_edit.h"
#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/map_element_fields.h"

class QMdiSubWindow;

namespace fastecu::ui
{

// A map subwindow's objectName() ("rom,map,name,type") parsed into its ROM
// and map index -- the same format read unguarded in five places today.
struct MapWindowId
{
    calibration::SessionId session{};
    int map_number{0};
};

// Returns nullopt for a null window, an object name with fewer than the two
// leading comma-separated fields this needs, or a first field that is not a
// session key.
std::optional<MapWindowId> parse_map_window_id(QMdiSubWindow *window);

// Owns definition fields and a typed snapshot for one edit operation. Keep the
// named ResolvedEdit alive while using the borrowed spec and cell span.
class ResolvedEdit
{
  public:
    calibration::MapElementSpec spec() const&
    {
        return fields_.spec();
    }
    calibration::MapElementSpec spec() const&& = delete;
    std::span<const calibration::NumericCell> cells() const&
    {
        return cells_.cells;
    }
    std::span<const calibration::NumericCell> cells() const&& = delete;
    const calibration::SelectionRange& range() const
    {
        return target_.range;
    }
    calibration::EditTargetKind kind() const
    {
        return target_.kind;
    }
    std::uint32_t x_size() const
    {
        return target_.x_size;
    }
    int map_number() const
    {
        return map_number_;
    }

  private:
    friend std::optional<ResolvedEdit> resolve_active_map_edit(QMdiSubWindow *, const calibration::CalibrationSession&,
                                                               int);
    ResolvedEdit(calibration::MapElementFields fields, calibration::EditTarget target, calibration::NumericRun cells,
                 int map_number);

    calibration::MapElementFields fields_;
    calibration::EditTarget target_;
    calibration::NumericRun cells_;
    int map_number_{0};
};

// Resolves the active map window's current selection to the element run it
// targets: reads the subwindow's QTableWidget selection, calls
// resolve_edit_target, and plucks the matching fields and numeric cells.
// Returns nullopt for a null window, a table widget that can't be found, an
// empty selection, or a rejected target (a static-axis scale type) -- the
// three cases the legacy per-function blocks handled with a bare `return`,
// plus the (practically unreachable) case of the table widget not being
// found.
std::optional<ResolvedEdit> resolve_active_map_edit(QMdiSubWindow *window, const calibration::CalibrationSession& def,
                                                    int map_number);

// Validates every retained cell byte range before writing session bytes.
// Invalid indices, target addresses, and byte widths reject the whole patch.
// Byte-identical writes are skipped, preserving clean state on complete no-ops.
Status apply_patch(calibration::CalibrationSession& def, int map_number, calibration::EditTargetKind kind,
                   const calibration::NumericEditPatch& patch);

} // namespace fastecu::ui
