#include "src/algorithms/checksum/qt_checksum.h"

#include "src/algorithms/protocol/qt_bytes.h"

namespace fastecu::checksum
{

std::uint8_t checksum8(const QByteArray& data, bool dec0x100)
{
    return checksum8(bytes::view(data), dec0x100);
}

} // namespace fastecu::checksum
