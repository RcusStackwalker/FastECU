#include "src/backend/dashboard/dashboard_document_service.h"

namespace fastecu::dashboard
{

Result<DashboardDocument> DashboardDocumentService::load(std::string_view handle) const
{
    auto bytes = repository_.read(handle);
    if (!bytes)
    {
        return std::unexpected(bytes.error());
    }
    return decode_dashboard_document(*bytes);
}

Status DashboardDocumentService::save(std::string_view handle, const DashboardDocument& document) const
{
    auto bytes = encode_dashboard_document(document);
    if (!bytes)
    {
        return std::unexpected(bytes.error());
    }
    return writer_.replace(handle, *bytes);
}

Result<DashboardDocument>
DashboardDocumentService::import_legacy_cdbg_catalog(std::string_view handle,
                                                     const LegacyCdbgImportDefaults& defaults) const
{
    auto bytes = repository_.read(handle);
    if (!bytes)
    {
        return std::unexpected(bytes.error());
    }
    return dashboard::import_legacy_cdbg_catalog(*bytes, defaults);
}

} // namespace fastecu::dashboard
