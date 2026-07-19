#include <gtest/gtest.h>

#include "protocol/qt_bytes.h"
#include "scripted_kline_transport.h"

using namespace mutdma;

TEST(TestTransport, scripted_write_then_read)
{
    ScriptedKlineTransport t;
    t.expectWrite(QByteArray::fromHex("A0"));
    t.queueRead(QByteArray::fromHex("A5"));
    ASSERT_EQ(t.setBaud(125000), true);
    ASSERT_EQ(t.write(bytes::view(QByteArray::fromHex("A0"))), 1);
    ASSERT_EQ(bytes::toQByteArray(t.read(50, -1)), QByteArray::fromHex("A5"));
    ASSERT_TRUE(t.scriptConsumed());
}

TEST(TestTransport, scripted_unexpected_write_flags)
{
    ScriptedKlineTransport t;
    t.expectWrite(QByteArray::fromHex("A0"));
    t.write(bytes::view(QByteArray::fromHex("BB")));
    ASSERT_FALSE(t.ok()); // mismatch recorded
}
