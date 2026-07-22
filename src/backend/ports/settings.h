#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace fastecu
{

// Replaces QSettings reads in backend.
class ISettings
{
  public:
    virtual ~ISettings() = default;
    virtual std::optional<std::string> get(std::string_view key) const = 0;
    virtual void set(std::string_view key, std::string_view value) = 0;
};

} // namespace fastecu
