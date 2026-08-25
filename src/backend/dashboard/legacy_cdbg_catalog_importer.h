#pragma once

#include <cstdint>
#include <string>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/ports/result.h"

namespace fastecu::dashboard
{
struct LegacyCdbgImportDefaults
{
    std::string document_name;
    std::uint32_t bitrate;
    CanIdentifierWidth identifier_width;
    std::uint8_t stream_instance;
    std::uint32_t sampling_interval_ms;
    RetryPolicy retry;
};

Result<DashboardDocument> import_legacy_cdbg_catalog(bytes::ByteView xml, const LegacyCdbgImportDefaults& defaults);
} // namespace fastecu::dashboard
