#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "src/backend/ports/result.h"

namespace fastecu
{

// Replaces direct Qt resource (":/config/", ":/kernels/") access. A future
// Kotlin/Android implementation reads from Android assets instead.
class IResourceBundle
{
  public:
    virtual ~IResourceBundle() = default;
    virtual Result<std::vector<std::string>> list(std::string_view bundle_id) = 0;
    virtual Result<std::vector<std::uint8_t>> read(std::string_view bundle_id, std::string_view name) = 0;
};

} // namespace fastecu
