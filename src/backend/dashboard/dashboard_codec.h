#pragma once

#include <cstdint>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/ports/result.h"

namespace fastecu::dashboard
{
Result<DashboardDocument> decode_dashboard_document(bytes::ByteView xml);
Result<std::vector<std::uint8_t>> encode_dashboard_document(const DashboardDocument& document);
} // namespace fastecu::dashboard
