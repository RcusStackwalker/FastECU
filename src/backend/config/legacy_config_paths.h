#pragma once
#include "src/backend/config/config_paths.h"
#include "src/backend/definitions/config_values.h"

namespace fastecu::config
{

// The single ConfigValuesStructure -> ConfigPaths conversion (issue #123).
// Extracted from two byte-identical anonymous-namespace copies that had grown
// in legacy_config_adapter.cpp and legacy_calibration_adapter.cpp.
//
// Deliberately takes ConfigValuesStructure fresh on every call rather than
// caching a ConfigPaths: FileActions's own methods take configValues fresh
// each time with no state threading them together, and callers may
// reasonably not have called set_base_dirs first.
//
// Qt-linked (ConfigValuesStructure's fields are QString), so this target is
// NOT in PORTABLE_ROOTS and must never be depended on by a portable target.
ConfigPaths paths_from_config_values(const fastecu::definitions::ConfigValuesStructure& values);

} // namespace fastecu::config
