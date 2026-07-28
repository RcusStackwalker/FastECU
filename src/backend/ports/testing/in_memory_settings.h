#pragma once
#include "src/backend/ports/settings.h"

#include <map>

namespace fastecu
{
class InMemorySettings : public ISettings
{
  public:
    std::optional<std::string> get(std::string_view k) const override
    {
        auto it = kv.find(std::string(k));
        if (it == kv.end())
            return std::nullopt;
        return it->second;
    }
    void set(std::string_view k, std::string_view v) override
    {
        kv[std::string(k)] = std::string(v);
    }
    std::map<std::string, std::string> kv;
};
} // namespace fastecu
