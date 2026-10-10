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
// user's settings, and the vehicle catalog. AppConfig::selected_vehicle_id
// is the only saved selection; everything about the selected vehicle is read
// from its VehicleSpec rather than cached.
class ConfigSession
{
  public:
    // `catalog` must outlive the session; initialize() reads it.
    ConfigSession(const Catalog& catalog, IFileSystem& file_system, IResourceBundle& resource_bundle,
                  IFileRepository& file_repository, IEventSink& events);
    ConfigSession(Catalog&& catalog, IFileSystem& file_system, IResourceBundle& resource_bundle,
                  IFileRepository& file_repository, IEventSink& events) = delete;
    ConfigSession(const ConfigSession&) = delete;
    ConfigSession& operator=(const ConfigSession&) = delete;

    // Provisions <app_root>/<version>/, loads settings, and validates the saved
    // vehicle id against the catalog: an id it does not know is cleared and
    // selects nothing. A failed rewrite of the loaded settings is a warning
    // event, not a failure. Any other failure leaves the session
    // uninitialized, holding nothing.
    Status Initialize(std::string_view app_root, std::string_view version);
    bool Initialized() const;

    // Writes the settings. On success the in-memory settings become the
    // normalized result; on failure they are left exactly as edited.
    Status Save();

    AppConfig& Settings();
    const AppConfig& Settings() const;

    // Where initialize() put things. Never moved by settings edits.
    ConfigPaths ProvisionedPaths() const;
    // The provisioned paths with the calibration and datalog directories
    // taken from settings (provisioned ones when a setting is empty).
    ConfigPaths EffectivePaths() const;

    // Catalog order. Empty until initialized.
    std::span<const VehicleSpec> Vehicles() const;
    Result<std::size_t> SelectedRow() const;
    // nullptr unless selected_row() has a value.
    const VehicleSpec *SelectedVehicle() const;

    // Makes `row`'s vehicle id the saved selection and sets the logging
    // protocol from its protocol. Transports are untouched, and nothing is written until
    // save(). An invalid row changes nothing.
    Status SelectRow(std::size_t row);
    // select_row() on the LAST row whose protocol is named `protocol_name`, as
    // the legacy ROM-open scan did. Returns false, changing nothing, if none do.
    bool SelectByProtocolName(std::string_view protocol_name);

    // The first vehicle whose protocol's alias is `flash_method`, which is how
    // a definition's flash method resolves; nullptr when none matches or the
    // session is not initialized.
    const VehicleSpec *VehicleForAlias(std::string_view flash_method) const;
    // The catalog protocol named `name`; nullptr when none is or the session
    // is not initialized.
    const ProtocolSpec *FindProtocol(std::string_view name) const;

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
