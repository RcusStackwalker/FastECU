#include "src/algorithms/checksum/qt_checksum.h"

#include <gtest/gtest.h>

TEST(TestQtChecksum, checksum8_matchesByteViewOverload)
{
    const QByteArray payload = QByteArray::fromHex("A800112233");
    ASSERT_EQ(fastecu::checksum::checksum8(payload, false), uint8_t(0x0E));
    ASSERT_EQ(fastecu::checksum::checksum8(payload, true), uint8_t(0xF2));
}
