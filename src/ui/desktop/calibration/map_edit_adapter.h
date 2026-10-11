#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <QString>

#include "src/ui/desktop/calibration/session_key.h"
#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/numeric_edit_use_case.h"

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
std::optional<MapWindowId> parseMapWindowId(QMdiSubWindow *window);

// Translates the window's first selected table range (row/column zero are
// axis headers where present) into the semantic target and element range a
// numeric edit expects. nullopt for a null window, a missing table, an empty
// selection, a session without the map, or a static-axis header selection.
std::optional<calibration::NumericSelection>
selectedNumericTarget(QMdiSubWindow *window, const calibration::CalibrationSession& session, int mapNumber);

// The widget cells a "select all" should cover for a numeric map's table of
// `rows` x `columns`: the body only, never the axis header row or column. It
// asks the same resolver that classifies edits where the body starts, so the
// two cannot disagree. nullopt when the session lacks the map or the table has
// no body cell.
std::optional<calibration::SelectionRange> bodyWidgetRange(const calibration::CalibrationSession& session,
                                                           int mapNumber, int rows, int columns);

// Splits clipboard text into owned rows of tab-separated text cells. Exactly
// one terminal LF is a record delimiter, not a row; empty cells, interior
// empty rows, ragged rows and carriage returns are kept for validation.
std::vector<std::vector<std::string>> splitPasteRows(const QString& text);

} // namespace fastecu::ui
