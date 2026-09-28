#pragma once
#include "src/backend/config/config_paths.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"
#include "src/backend/ports/resource_bundle.h"
#include "src/backend/ports/result.h"

namespace fastecu::config
{

// Replaces FileActions::check_config_dirs. Creates every directory
// ConfigPaths names if missing, migrates fastecu.cfg forward from the newest
// previous-version directory, writes any bundled default config/kernel file
// not already present, and prunes syslogs down to the newest 20. Bundled
// files are read through `resource_bundle` and written through
// `file_repository`; a failure to do either names the target file.
// `file_system` and `file_repository` must refer to the same backing storage:
// existence checks and migration copies must be visible to repository reads.
Status provision_config_directories(const ConfigPaths& paths, IFileSystem& file_system,
                                    IResourceBundle& resource_bundle, IFileRepository& file_repository,
                                    IEventSink& events);

} // namespace fastecu::config
