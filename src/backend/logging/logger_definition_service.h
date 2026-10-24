#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/logging/logger_conf.h"
#include "src/backend/logging/logger_definition_model.h"
#include "src/backend/logging/logger_definition_parser.h"
#include "src/backend/ports/atomic_file_writer.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

// Owns handle resolution and the read -> parse -> maybe-write composition for
// the logger definition and conf files. Portable: every I/O path goes through
// an injected port.
class LoggerDefinitionService
{
  public:
    LoggerDefinitionService(IFileRepository&, IAtomicFileWriter&);

    // Returns `configured_handle` unchanged when it is non-empty. When it is
    // empty and `log_protocol` is "CDBG", prefers
    // <config_files_directory>logger_cdbg_example.xml and falls back to the
    // bundled resource. Otherwise returns an empty handle -- the caller
    // reports "no logger definition file selected". Resolution cannot fail.
    std::string ResolveDefinitionHandle(std::string_view configured_handle, std::string_view log_protocol,
                                        std::string_view config_files_directory);

    Result<LoggerDefinition> LoadDefinition(std::string_view handle);

    // Reads the conf file's selection for `ecu_id`. Returns an empty optional
    // when that ECU has no entry, and never writes. A caller that wants to
    // initialize a missing entry persists the default with SaveSelection.
    Result<std::optional<LoggerSelection>> LoadSelection(std::string_view conf_handle, std::string_view ecu_id);

    Status SaveSelection(std::string_view conf_handle, std::string_view ecu_id, const LoggerSelection& selection);

  private:
    IFileRepository& repository_;
    IAtomicFileWriter& writer_;
};

} // namespace fastecu::logging
