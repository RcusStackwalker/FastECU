#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <QString>

#include "src/backend/calibration/map_edit.h"
#include "src/backend/definitions/ecu_cal_def.h"

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
    friend MapElementFields collect_map_element_fields(const definitions::EcuCalDefStructure&, int,
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
    std::uint64_t rom_file_size_{0};
};

// Plucks one element run's fields out of the Qt-typed model. `kind` selects
// between the map-body lists, the XScale* lists, and the YScale* lists -- the
// field-plucking half of what the three duplicated legacy blocks did.
//
// Takes fastecu::definitions::EcuCalDefStructure rather than
// FileActions::EcuCalDefStructure (the same type, via FileActions's `using
// EcuCalDefStructure = fastecu::definitions::EcuCalDefStructure` alias) so
// this package can depend on the lightweight //src/backend/definitions:ecu_cal_def
// target instead of pulling in the full legacy FileActions god object.
// Callers holding a FileActions::EcuCalDefStructure pass it in unchanged.
MapElementFields collect_map_element_fields(const definitions::EcuCalDefStructure& def, int map_number,
                                            calibration::EditTargetKind kind);

// Formats read_raw_element's raw value the same way the deleted legacy
// get_rom_data_value did: unsigned as a plain decimal, signed 8/16/32
// sign-extended, float reinterpreted through its bit pattern, and -- an
// unrecognized storage type (spec.storage_type is std::nullopt) or 3-byte
// signed storage (int24) -- an empty string, matching get_rom_data_value's
// startsWith("float"/"uint"/"int") chain, which assigns `value` in none of
// those cases (see PinnedDefect_Int24AlwaysReadsAsZero in map_edit_test.cpp
// for the int24 half of this).
QString format_raw_element_value(const calibration::MapElementSpec& spec, std::int64_t raw);

// A map subwindow's objectName() ("rom,map,name,type") parsed into its ROM
// and map index -- the same format read unguarded in five places today.
struct MapWindowId
{
    int rom_number{0};
    int map_number{0};
};

// Returns nullopt for a null window or an object name with fewer than the
// two leading comma-separated fields this needs.
std::optional<MapWindowId> parse_map_window_id(QMdiSubWindow *window);

// Everything an edit operation needs about the active map window, resolved
// once. Owns the MapElementFields and the split cell text so the views
// handed out by spec() and cell_text() stay valid for the caller's whole
// statement -- callers must keep the ResolvedEdit alive (a named local,
// e.g. `auto edit = resolve_active_map_edit(...)`) across the whole edit
// operation.
class ResolvedEdit
{
  public:
    calibration::MapElementSpec spec() const&
    {
        return fields_.spec();
    }
    calibration::MapElementSpec spec() const&& = delete;
    std::span<const std::string_view> cell_text() const
    {
        return cell_text_;
    }
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
    friend std::optional<ResolvedEdit> resolve_active_map_edit(QMdiSubWindow *, const definitions::EcuCalDefStructure&,
                                                               int);

    // MapElementFields's default constructor is private (only
    // collect_map_element_fields may default-construct one); ResolvedEdit
    // therefore cannot rely on an implicit default constructor either, since
    // that would need to default-construct fields_ with no access to do so.
    // This constructor sidesteps that: it takes an already-built
    // MapElementFields and uses its implicitly-generated (and therefore
    // public, unaffected by the private default ctor) move constructor
    // instead.
    ResolvedEdit(MapElementFields fields, calibration::EditTarget target, std::vector<std::string> owned_cell_text,
                 int map_number);

    MapElementFields fields_;
    calibration::EditTarget target_;
    std::vector<std::string> owned_cell_text_;
    std::vector<std::string_view> cell_text_;
    int map_number_{0};
};

// Resolves the active map window's current selection to the element run it
// targets: reads the subwindow's QTableWidget selection, calls
// resolve_edit_target, and plucks the matching field group + cell text.
// Returns nullopt for a null window, a table widget that can't be found, an
// empty selection, or a rejected target (a static-axis scale type) -- the
// three cases the legacy per-function blocks handled with a bare `return`,
// plus the (practically unreachable) case of the table widget not being
// found.
std::optional<ResolvedEdit> resolve_active_map_edit(QMdiSubWindow *window, const definitions::EcuCalDefStructure& def,
                                                    int map_number);

} // namespace fastecu::ui
