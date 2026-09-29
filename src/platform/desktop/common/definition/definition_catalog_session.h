#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/calibration/session/definition_catalogs.h"
#include "src/backend/config/config_session.h"
#include "src/backend/definition/definition_service.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/file_system.h"

namespace fastecu::desktop::definition
{

// Desktop catalog sources and startup lookup provenance. The composition root
// owns this session and keeps its borrowed services alive until consumers stop.
class DefinitionCatalogSession final : public calibration::IDefinitionCatalogs
{
  public:
    DefinitionCatalogSession(fastecu::definition::DefinitionService& definitions, config::ConfigSession& config,
                             IFileSystem& file_system, IEventSink& events);

    Result<fastecu::definition::DefinitionCatalog> catalog(fastecu::definition::DefinitionFormat format) override;
    std::optional<std::string> indexed_source(fastecu::definition::DefinitionFormat format,
                                              std::string_view id) override;
    Status refresh_index(fastecu::definition::DefinitionFormat format);

  private:
    struct IndexedSource
    {
        std::string definition_id;
        std::string source;
    };

    std::vector<IndexedSource>& index(fastecu::definition::DefinitionFormat format);
    void log_error(std::string_view operation, const Error& error);

    fastecu::definition::DefinitionService& definitions_;
    config::ConfigSession& config_;
    IFileSystem& file_system_;
    IEventSink& events_;
    std::vector<IndexedSource> ecuflash_index_;
    std::vector<IndexedSource> romraider_index_;
    std::vector<std::string> submitted_ecuflash_handles_;
};

} // namespace fastecu::desktop::definition
