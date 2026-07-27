#pragma once
#include "src/backend/ports/resource_bundle.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fastecu
{
class InMemoryResourceBundle : public IResourceBundle
{
  public:
    Result<std::vector<std::string>> list(std::string_view bundle_id) override
    {
        auto it = bundles.find(std::string(bundle_id));
        if (it == bundles.end())
            return fastecu::fail(ErrorKind::InvalidConfig, "no such bundle");
        std::vector<std::string> names;
        for (auto& [name, bytes] : it->second)
            names.push_back(name);
        return names;
    }
    Result<std::vector<std::uint8_t>> read(std::string_view bundle_id, std::string_view name) override
    {
        auto bundle_it = bundles.find(std::string(bundle_id));
        if (bundle_it == bundles.end())
            return fastecu::fail(ErrorKind::InvalidConfig, "no such bundle");
        auto file_it = bundle_it->second.find(std::string(name));
        if (file_it == bundle_it->second.end())
            return fastecu::fail(ErrorKind::InvalidConfig, "no such file");
        return file_it->second;
    }

    std::map<std::string, std::map<std::string, std::vector<std::uint8_t>>> bundles;
};
} // namespace fastecu
