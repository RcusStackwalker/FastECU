#pragma once

#include <cstddef>
#include <optional>
#include <set>

#include <QString>

namespace fastecu::ui
{

// Presentation state of one open calibration: which map windows are open,
// which data-tree categories are expanded, and whether the operator chose
// "continue without definition" (and with which vehicle make). Legacy kept
// these as VisibleList / CategoryExpandedList / RomInfoExpanded / RomInfo
// placeholders inside EcuCalDefStructure; they are UI state, not calibration
// data.
struct CalibrationViewState
{
    std::set<std::size_t> open_maps;
    std::set<QString> expanded_categories;
    bool rom_info_expanded{false};
    std::optional<QString> missing_definition_make;
};

} // namespace fastecu::ui
