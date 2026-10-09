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

namespace fastecu::definition
{

// Catalog sources and startup lookup provenance. The composition root
// owns this session and keeps its borrowed services alive until consumers stop.
class DefinitionCatalogSession final : public calibration::IDefinitionCatalogs
{
  public:
    DefinitionCatalogSession(fastecu::definition::DefinitionService& definitions, config::ConfigSession& config,
                             IFileSystem& file_system, IEventSink& events);

    Result<fastecu::definition::DefinitionCatalog> Catalog(fastecu::definition::DefinitionFormat format) override;
    std::optional<std::string> IndexedSource(fastecu::definition::DefinitionFormat format,
                                             std::string_view id) override;
    Status RefreshIndex(fastecu::definition::DefinitionFormat format);
    Status SubmitNewDefinition(std::string_view destination, const fastecu::definition::DefinitionHeaderInput& input,
                               bool allow_overwrite);
    Status SubmitImportedDefinition(std::string_view source, std::string_view destination,
                                    const fastecu::definition::DefinitionHeaderInput& input);

  private:
    struct IndexEntry
    {
        std::string definition_id;
        std::string source;
    };

    std::vector<IndexEntry>& Index(fastecu::definition::DefinitionFormat format);
    void LogError(std::string_view operation, const Error& error);
    void RememberSubmission(std::string_view destination, std::string_view id);

    fastecu::definition::DefinitionService& definitions_;
    config::ConfigSession& config_;
    IFileSystem& file_system_;
    IEventSink& events_;
    std::vector<IndexEntry> ecuflash_index_;
    std::vector<IndexEntry> romraider_index_;
    std::vector<std::string> submitted_ecuflash_handles_;
};

} // namespace fastecu::definition
