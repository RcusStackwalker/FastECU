#pragma once
#include <expected>
#include <string>
#include <utility>
#include "src/backend/ports/error.h"

namespace fastecu
{

template <class T>
using Result = std::expected<T, Error>;

using Status = std::expected<void, Error>;

inline std::unexpected<Error> fail(ErrorKind k, std::string detail = {})
{
    return std::unexpected(Error{k, std::move(detail)});
}

} // namespace fastecu
