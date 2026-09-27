#pragma once
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "src/backend/config/app_config.h"
#include "src/backend/config/car_model_catalog.h"
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
// is the only saved selection; everything about the selected vehicle is
// derived from its ResolvedCarModel rather than cached.
class ConfigSession
{
  public:
    ConfigSession(IFileSystem& file_system, IResourceBundle& resource_bundle, IFileRepository& file_repository,
                  IEventSink& events);
    ConfigSession(const ConfigSession&) = delete;
    ConfigSession& operator=(const ConfigSession&) = delete;

    // Provisions <app_root>/<version>/, loads settings and both catalogs, and
    // validates the saved row (an invalid one becomes "0"). A failed rewrite
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

    // File order; a row's id is its position.
    std::span<const ResolvedCarModel> vehicles() const;
    Result<std::size_t> selected_row() const;
    // nullptr unless selected_row() has a value.
    const ResolvedCarModel *selected_vehicle() const;

  private:
    IFileSystem& file_system_;
    IResourceBundle& resource_bundle_;
    IFileRepository& file_repository_;
    IEventSink& events_;

    bool initialized_ = false;
    ConfigPaths provisioned_;
    AppConfig settings_;
    std::vector<ResolvedCarModel> vehicles_;
};

} // namespace fastecu::config
