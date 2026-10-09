#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include "src/backend/ports/result.h"

namespace fastecu
{

class IAtomicFileWriter
{
  public:
    virtual ~IAtomicFileWriter() = default;
    virtual Status Replace(std::string_view handle, std::span<const std::uint8_t> data) = 0;
};

} // namespace fastecu
