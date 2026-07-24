#pragma once
#include "src/backend/ports/resource_bundle.h"

// Qt-resource-backed IResourceBundle. Bundle id "config" maps to the
// ":/config/" prefix from resources/shared/config.qrc; "kernels" maps to
// ":/kernels/" from resources/shared/kernels.qrc.
class QtResourceBundle : public fastecu::IResourceBundle
{
  public:
    fastecu::Result<std::vector<std::string>> list(std::string_view bundle_id) override;
    fastecu::Result<std::vector<std::uint8_t>> read(std::string_view bundle_id, std::string_view name) override;
};
