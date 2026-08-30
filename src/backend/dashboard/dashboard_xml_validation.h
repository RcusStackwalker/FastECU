#pragma once

#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/ports/result.h"

namespace fastecu::dashboard
{
Status validate_xml_text(std::string_view value, std::string path);
Status validate_xml_input(bytes::ByteView xml, std::string path);
Status validate_dashboard_document_xml_strings(const DashboardDocument& document);
} // namespace fastecu::dashboard
