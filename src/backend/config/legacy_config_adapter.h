#pragma once
#include "src/backend/config/app_config.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/config/protocol_catalog.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"
#include "src/backend/ports/resource_bundle.h"

namespace fastecu::config
{

// Bridges the portable config/settings use cases back into
// FileActions::ConfigValuesStructure so every existing MainWindow call site
// keeps working unchanged. FileActions's five converted methods each become a
// one-line delegation to the corresponding method here.
class LegacyConfigAdapter
{
  public:
    LegacyConfigAdapter(IFileSystem& file_system, IResourceBundle& resource_bundle,
                        IFileRepository& file_repository);

    FileActions::ConfigValuesStructure *set_base_dirs(FileActions::ConfigValuesStructure *values,
                                                      const AppRootInfo& root_info);
    FileActions::ConfigValuesStructure *check_config_dirs(FileActions::ConfigValuesStructure *values);
    FileActions::ConfigValuesStructure *read_config_file(FileActions::ConfigValuesStructure *values);
    FileActions::ConfigValuesStructure *save_config_file(FileActions::ConfigValuesStructure *values);
    FileActions::ConfigValuesStructure *read_protocols_file(FileActions::ConfigValuesStructure *values);

  private:
    IFileSystem& file_system_;
    IResourceBundle& resource_bundle_;
    IFileRepository& file_repository_;
};

} // namespace fastecu::config
