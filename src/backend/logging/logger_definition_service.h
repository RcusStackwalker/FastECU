#pragma once

#include <string>
#include <string_view>

#include "src/backend/logging/logger_conf.h"
#include "src/backend/logging/logger_definition_model.h"
#include "src/backend/logging/logger_definition_parser.h"
#include "src/backend/ports/atomic_file_writer.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/resource_bundle.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

// Owns handle resolution and the read -> parse -> maybe-write composition for
// the logger definition and conf files. Portable: every I/O path goes through
// an injected port.
class LoggerDefinitionService
{
  public:
    LoggerDefinitionService(IFileRepository&, IResourceBundle&, IAtomicFileWriter&);

    // Returns `configured_handle` unchanged when it is non-empty. When it is
    // empty and `log_protocol` is "CDBG", prefers
    // <config_files_directory>logger_cdbg_example.xml and falls back to the
    // bundled resource. Otherwise returns an empty handle -- the caller
    // reports "no logger definition file selected".
    Result<std::string> resolve_definition_handle(
        std::string_view configured_handle,
        std::string_view log_protocol,
        std::string_view config_files_directory);

    Result<LoggerDefinition> load_definition(std::string_view handle);

    // Reads the conf file's selection for `ecu_id`. When that ECU has no
    // entry, composes default_selection(definition) and persists it, so the
    // write is an explicit step rather than a side effect of a read.
    Result<LoggerSelection> load_or_initialize_selection(
        std::string_view conf_handle,
        std::string_view ecu_id,
        const LoggerDefinition& definition);

    Status save_selection(
        std::string_view conf_handle,
        std::string_view ecu_id,
        const LoggerSelection& selection);

  private:
    IFileRepository& repository_;
    IResourceBundle& bundle_;
    IAtomicFileWriter& writer_;
};

} // namespace fastecu::logging
