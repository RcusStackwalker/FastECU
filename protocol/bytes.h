#ifndef FASTECU_BYTES_H
#define FASTECU_BYTES_H

#include <cstdint>
#include <span>
#include <vector>

namespace bytes {

using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;
using ByteView = std::span<const Byte>;

} // namespace bytes

#endif // FASTECU_BYTES_H
