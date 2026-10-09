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
    Result<std::string> ResolveDefinitionHandle(std::string_view configured_handle, std::string_view log_protocol,
                                                std::string_view config_files_directory);

    Result<LoggerDefinition> LoadDefinition(std::string_view handle);

    // Reads the conf file's selection for `ecu_id`. Returns an empty optional
    // when that ECU has no entry, and never writes -- a caller with no
    // definition to initialize from can ask without persisting anything.
    Result<std::optional<LoggerSelection>> LoadSelection(std::string_view conf_handle, std::string_view ecu_id);

    // load_selection, plus: when that ECU has no entry, composes
    // the explicit fallback and persists it, so the write is an
    // explicit step rather than a side effect of a read.
    Result<LoggerSelection> LoadOrInitializeSelection(std::string_view conf_handle, std::string_view ecu_id,
                                                      const LoggerSelection& fallback);

    Status SaveSelection(std::string_view conf_handle, std::string_view ecu_id, const LoggerSelection& selection);

  private:
    // The one read both public loaders share. `conf_out` receives the bytes so
    // load_or_initialize_selection can hand them to write_selection without a
    // second trip through the repository -- write_selection appends to the
    // document it was given, so it needs the same bytes the parse saw.
    Result<std::optional<LoggerSelection>> LoadSelection(std::string_view conf_handle, std::string_view ecu_id,
                                                         bytes::Bytes& conf_out);

    IFileRepository& repository_;
    // Held per the plan's constructor signature, but unused: the bundled
    // ":/config/..." handle is resolved by the repository (QFile natively
    // handles ":/"), not read through the bundle port directly.
    [[maybe_unused]] IResourceBundle& bundle_;
    IAtomicFileWriter& writer_;
};

} // namespace fastecu::logging
