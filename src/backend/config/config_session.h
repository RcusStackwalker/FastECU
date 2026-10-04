#pragma once
#include <cstddef>
#include <span>
#include <string_view>

#include "src/backend/config/app_config.h"
#include "src/backend/config/catalog.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"
#include "src/backend/ports/resource_bundle.h"
#include "src/backend/ports/result.h"

namespace fastecu::config
{

// The application's configuration for one run: provisioned paths, the
// user's settings, and the vehicle catalog. AppConfig::selected_protocol_id
// is the only saved selection; everything about the selected vehicle is read
// from its VehicleSpec rather than cached.
class ConfigSession
{
  public:
    // `catalog` must outlive the session; initialize() reads it.
    ConfigSession(const Catalog& catalog, IFileSystem& file_system, IResourceBundle& resource_bundle,
                  IFileRepository& file_repository, IEventSink& events);
    ConfigSession(const ConfigSession&) = delete;
    ConfigSession& operator=(const ConfigSession&) = delete;

    // Provisions <app_root>/<version>/, loads settings, and validates the saved
    // row against the catalog (an invalid one becomes "0"). A failed rewrite
    // of the loaded settings is a warning event, not a failure. Any other
    // failure leaves the session uninitialized, holding nothing.
    Status initialize(std::string_view app_root, std::string_view version);
    bool initialized() const;

    // Writes the settings. On success the in-memory settings become the
    // normalized result; on failure they are left exactly as edited.
    Status save();

    AppConfig& settings();
    const AppConfig& settings() const;

    // Where initialize() put things. Never moved by settings edits.
    ConfigPaths provisioned_paths() const;
    // The provisioned paths with the calibration and datalog directories
    // taken from settings (provisioned ones when a setting is empty).
    ConfigPaths effective_paths() const;

    // Catalog order; a row's id is its position. Empty until initialized.
    std::span<const VehicleSpec> vehicles() const;
    Result<std::size_t> selected_row() const;
    // nullptr unless selected_row() has a value.
    const VehicleSpec *selected_vehicle() const;

    // Makes `row` the saved row and sets the logging protocol from its
    // protocol. Transports are untouched, and nothing is written until
    // save(). An invalid row changes nothing.
    Status select_row(std::size_t row);
    // select_row() on the LAST row whose protocol is named `protocol_name`, as
    // the legacy ROM-open scan did. Returns false, changing nothing, if none do.
    bool select_by_protocol_name(std::string_view protocol_name);

    // The first vehicle whose protocol's alias is `flash_method`, which is how
    // a definition's flash method resolves; nullptr when none matches or the
    // session is not initialized.
    const VehicleSpec *vehicle_for_alias(std::string_view flash_method) const;

  private:
    const Catalog& catalog_;
    IFileSystem& file_system_;
    IResourceBundle& resource_bundle_;
    IFileRepository& file_repository_;
    IEventSink& events_;

    bool initialized_ = false;
    ConfigPaths provisioned_;
    AppConfig settings_;
};

} // namespace fastecu::config
