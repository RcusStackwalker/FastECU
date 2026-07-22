#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include "src/backend/ports/result.h"

namespace fastecu
{

// Replaces QFile / QFileDialog access in backend. A "handle" is an opaque
// repository key the platform resolves to a real path or resource.
class IFileRepository
{
  public:
    virtual ~IFileRepository() = default;
    virtual Result<std::vector<std::uint8_t>> read(std::string_view handle) = 0;
    virtual Status write(std::string_view handle, std::span<const std::uint8_t>) = 0;
};

} // namespace fastecu
