#pragma once

#include <string_view>

#include "src/backend/dashboard/dashboard_codec.h"
#include "src/backend/dashboard/legacy_cdbg_catalog_importer.h"
#include "src/backend/ports/atomic_file_writer.h"
#include "src/backend/ports/file_repository.h"

namespace fastecu::dashboard
{

// Stateless composition boundary for dashboard persistence. Each operation
// receives its handle and document explicitly; all I/O goes through its ports.
class DashboardDocumentService
{
  public:
    DashboardDocumentService(IFileRepository& repository, IAtomicFileWriter& writer)
        : repository_(repository), writer_(writer)
    {
    }

    Result<DashboardDocument> load(std::string_view handle) const;
    Status save(std::string_view handle, const DashboardDocument& document) const;
    Result<DashboardDocument> import_legacy_cdbg_catalog(std::string_view handle,
                                                         const LegacyCdbgImportDefaults& defaults) const;

  private:
    IFileRepository& repository_;
    IAtomicFileWriter& writer_;
};

} // namespace fastecu::dashboard
