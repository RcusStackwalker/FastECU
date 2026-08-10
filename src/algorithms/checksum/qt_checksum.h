#pragma once

#include "src/algorithms/checksum/checksum_primitives.h"

#include <QByteArray>

#include <cstdint>

namespace fastecu::checksum
{

// TRANSITIONAL Qt shim: QByteArray overload delegating to the portable
// checksum8(bytes::ByteView, bool) above.
std::uint8_t checksum8(const QByteArray& data, bool dec0x100 = false);

} // namespace fastecu::checksum
