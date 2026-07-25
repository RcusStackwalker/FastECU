#pragma once
#include "src/backend/config/app_config.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/config/protocol_catalog.h"
#include "src/backend/definitions/config_values.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"
#include "src/backend/ports/resource_bundle.h"

namespace fastecu::config
{

// Bridges the portable config/settings use cases back into
// FileActions::ConfigValuesStructure so every existing MainWindow call site
// keeps working unchanged. FileActions's five converted methods each become a
// one-line delegation to the corresponding method here.
//
// Uses fastecu::definitions::ConfigValuesStructure (config_values.h) rather
// than including file_actions.h directly: FileActions owns a
// LegacyConfigAdapter member, so file_actions.h -> legacy_config_adapter.h ->
// file_actions.h would be circular (and the matching //src/backend/definitions
// <-> //src/backend/config:legacy_config_adapter Bazel deps would form a hard
// cycle). fastecu::definitions::ConfigValuesStructure is exactly the type
// FileActions::ConfigValuesStructure aliases, so this is not a behavior
// change -- see config_values.h's comment.
class LegacyConfigAdapter
{
  public:
    LegacyConfigAdapter(IFileSystem& file_system, IResourceBundle& resource_bundle,
                        IFileRepository& file_repository);

    fastecu::definitions::ConfigValuesStructure *set_base_dirs(
        fastecu::definitions::ConfigValuesStructure *values, const AppRootInfo& root_info);
    fastecu::definitions::ConfigValuesStructure *check_config_dirs(
        fastecu::definitions::ConfigValuesStructure *values);
    fastecu::definitions::ConfigValuesStructure *read_config_file(
        fastecu::definitions::ConfigValuesStructure *values);
    fastecu::definitions::ConfigValuesStructure *save_config_file(
        fastecu::definitions::ConfigValuesStructure *values);
    fastecu::definitions::ConfigValuesStructure *read_protocols_file(
        fastecu::definitions::ConfigValuesStructure *values);

  private:
    IFileSystem& file_system_;
    IResourceBundle& resource_bundle_;
    IFileRepository& file_repository_;
};

} // namespace fastecu::config
