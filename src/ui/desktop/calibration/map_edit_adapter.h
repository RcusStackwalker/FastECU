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

class QMdiSubWindow;

namespace fastecu::ui
{

// Owns the std::strings a MapElementSpec's string_views point at. A spec built
// directly from QString::toStdString() temporaries dangles the moment the
// statement ends -- the single easiest way to get this wrong.
//
// Copy/move are left as compiler defaults, not restricted: a short string
// (endian_, to_byte_, ...) typically lives inline in this object under small-
// string optimization, so a copy OR A MOVE relocates its bytes to a new
// address, dangling a spec() already taken from the source -- restricting
// only copy would not close that hazard (move has the same problem), and
// forbidding both is not viable here: collect_map_element_fields below
// returns a named local across multiple branches, which needs a move
// constructor whenever NRVO doesn't apply (not guaranteed by the standard;
// deleting move made this fail to compile). The real invariant this class
// enforces is narrower and IS fully closed: never call spec() on an object
// whose lifetime has already ended or is about to. spec()'s ref-qualification
// below closes the temporary case (`collect_map_element_fields(...).spec()`)
// at compile time; the "reassigned/moved after spec() was taken" case is not
// possible with any current call site (each binds `fields` once via `auto`
// and never mutates or relocates it), so it is left as a documented
// discipline rather than a mechanism.
class MapElementFields
{
  public:
    // Lvalue-only: `collect_map_element_fields(...).spec()` would hand back a
    // spec whose string_views point into a temporary that is gone by the
    // semicolon, so that call is a compile error instead of a dangle. Callers
    // must bind the temporary first (`auto fields = ...; auto spec =
    // fields.spec();`) and keep `fields` alive as long as `spec` is used.
    calibration::MapElementSpec spec() const&;
    calibration::MapElementSpec spec() const&& = delete;

  private:
    friend MapElementFields collect_map_element_fields(const calibration::CalibrationSession&, int,
                                                       calibration::EditTargetKind);

    MapElementFields() = default;

    std::string endian_;
    std::string to_byte_;
    std::string from_byte_;
    std::string min_value_;
    std::string max_value_;
    std::string flash_method_;
    std::uint64_t address_{0};
    std::optional<definition::StorageType> storage_type_;
    double coarse_increment_{0.0};
    double fine_increment_{0.0};
    std::uint32_t x_size_{1};
    std::uint32_t y_size_{1};
    std::uint32_t start_position_{1};
    std::uint32_t interval_{1};
    std::uint64_t rom_file_size_{0};
};

// Collects the resolved definition and protocol fields for one element run.
MapElementFields collect_map_element_fields(const calibration::CalibrationSession& def, int map_number,
                                            calibration::EditTargetKind kind);

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
    ResolvedEdit(MapElementFields fields, calibration::EditTarget target, calibration::NumericRun cells,
                 int map_number);

    MapElementFields fields_;
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
