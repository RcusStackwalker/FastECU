#pragma once
#include "src/backend/config/config_paths.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/file_system.h"
#include "src/backend/ports/resource_bundle.h"
#include "src/backend/ports/result.h"

namespace fastecu::config
{

// Replaces FileActions::check_config_dirs. Creates every directory
// ConfigPaths names if missing, migrates fastecu.cfg forward from the newest
// previous-version directory, copies any bundled default config/kernel file
// not already present, and prunes syslogs down to the newest 20.
Status provision_config_directories(const ConfigPaths& paths, IFileSystem& file_system,
                                    IResourceBundle& resource_bundle, IEventSink& events);

} // namespace fastecu::config
