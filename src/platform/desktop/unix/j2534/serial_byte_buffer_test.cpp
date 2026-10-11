#include "src/platform/desktop/unix/j2534/serial_byte_buffer.h"

#include <gtest/gtest.h>

#include <deque>
#include <vector>

namespace
{

// Drives SerialByteBuffer with scripted poll results and virtual time, so the
// silence-timeout rules are asserted deterministically rather than by sleeping.
class FakeSource
{
  public:
    void Queue(QByteArray chunk)
    {
        chunks_.push_back(std::move(chunk));
    }

    SerialByteBuffer Make()
    {
        return SerialByteBuffer([this] { return Poll(); }, [this](int ms) { Wait(ms); }, [this] { return now; });
    }

    std::uint64_t now = 0;
    std::vector<int> waits;
    int polls = 0;

  private:
    QByteArray Poll()
    {
        ++polls;
        if (chunks_.empty())
        {
            return {};
        }
        QByteArray chunk = std::move(chunks_.front());
        chunks_.pop_front();
        return chunk;
    }

    void Wait(int ms)
    {
        waits.push_back(ms);
        now += static_cast<std::uint64_t>(ms);
    }

    std::deque<QByteArray> chunks_;
};

TEST(SerialByteBuffer, ServesAnExactFitFromASingleRefill)
{
    FakeSource source;
    source.Queue(QByteArray("abc"));
    SerialByteBuffer buffer = source.Make();

    EXPECT_EQ(buffer.Take(3, 100), QByteArray("abc"));
    EXPECT_EQ(buffer.Buffered(), 0U);
}

TEST(SerialByteBuffer, RetainsBytesReadPastTheRequestedCount)
{
    FakeSource source;
    source.Queue(QByteArray("abcdef"));
    SerialByteBuffer buffer = source.Make();

    EXPECT_EQ(buffer.Take(2, 100), QByteArray("ab"));
    EXPECT_EQ(buffer.Buffered(), 4U);
    const int polls_before_second_take = source.polls;
    // The retained bytes are served without touching the source again.
    EXPECT_EQ(buffer.Take(4, 100), QByteArray("cdef"));
    EXPECT_EQ(source.polls, polls_before_second_take);
}

TEST(SerialByteBuffer, ServesFromTheBufferWithoutWaitingWhenItAlreadyHasEnough)
{
    FakeSource source;
    source.Queue(QByteArray("abcdef"));
    SerialByteBuffer buffer = source.Make();

    buffer.Take(1, 100);
    source.waits.clear();
    buffer.Take(1, 100);

    // This is the whole point of the class: a satisfiable one-byte read must
    // not cost an event-loop wait.
    EXPECT_TRUE(source.waits.empty());
}

TEST(SerialByteBuffer, AccumulatesAcrossSeveralRefills)
{
    FakeSource source;
    source.Queue(QByteArray("ab"));
    source.Queue(QByteArray("cd"));
    SerialByteBuffer buffer = source.Make();

    EXPECT_EQ(buffer.Take(4, 100), QByteArray("abcd"));
}

TEST(SerialByteBuffer, ReturnsWhatItHasWhenTheSourceGoesSilent)
{
    FakeSource source;
    source.Queue(QByteArray("ab"));
    SerialByteBuffer buffer = source.Make();

    // Asked for 5, only 2 ever arrive.
    EXPECT_EQ(buffer.Take(5, 10), QByteArray("ab"));
}

TEST(SerialByteBuffer, ReturnsEmptyWhenNothingEverArrives)
{
    FakeSource source;
    SerialByteBuffer buffer = source.Make();

    EXPECT_EQ(buffer.Take(4, 10), QByteArray());
}

TEST(SerialByteBuffer, TheDeadlineRefreshesOnEveryArrival)
{
    FakeSource source;
    // Four silent 1 ms waits, then a byte, repeated three times. With a 5 ms
    // silence timeout and no refresh this would give up long before the last
    // byte.
    for (int i = 0; i < 3; ++i)
    {
        for (int silent = 0; silent < 4; ++silent)
        {
            source.Queue(QByteArray());
        }
        source.Queue(QByteArray("x"));
    }
    SerialByteBuffer buffer = source.Make();

    EXPECT_EQ(buffer.Take(3, 5), QByteArray("xxx"));
}

TEST(SerialByteBuffer, WaitsOneMillisecondAtATimeWhenSilent)
{
    FakeSource source;
    SerialByteBuffer buffer = source.Make();

    // Nothing ever arrives, so every iteration silently waits until the
    // 3 ms timeout expires. A wait_(0) busy-spin would also pass every other
    // test in this file while burning a core in production, so pin the
    // argument value explicitly.
    buffer.Take(1, 3);

    ASSERT_FALSE(source.waits.empty());
    for (int ms : source.waits)
    {
        EXPECT_EQ(ms, 1);
    }
}

TEST(SerialByteBuffer, ZeroLengthRequestReturnsImmediately)
{
    FakeSource source;
    SerialByteBuffer buffer = source.Make();

    EXPECT_EQ(buffer.Take(0, 1000), QByteArray());
    EXPECT_TRUE(source.waits.empty());
}

TEST(SerialByteBuffer, ClearDiscardsRetainedBytes)
{
    FakeSource source;
    source.Queue(QByteArray("abcdef"));
    SerialByteBuffer buffer = source.Make();

    buffer.Take(1, 100);
    ASSERT_EQ(buffer.Buffered(), 5U);
    buffer.Clear();

    EXPECT_EQ(buffer.Buffered(), 0U);
    EXPECT_EQ(buffer.Take(1, 1), QByteArray());
}

} // namespace
