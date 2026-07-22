#include "src/backend/ports/clock.h"
#include "src/backend/ports/cancellation.h"
#include <gtest/gtest.h>

using fastecu::ErrorKind;
using fastecu::ICancellationToken;
using fastecu::IClock;
using fastecu::Status;

namespace
{
// A deterministic clock for tests: now advances only when told; sleep is
// instantaneous but honours the token.
class FakeClock : public IClock
{
  public:
    std::uint64_t now_ms() const override
    {
        return now_;
    }
    Status sleep(int ms, const ICancellationToken& t) override
    {
        if (t.cancelled())
            return fastecu::fail(ErrorKind::Cancelled);
        now_ += static_cast<std::uint64_t>(ms < 0 ? 0 : ms);
        return {};
    }
    std::uint64_t now_ = 0;
};

class FlagToken : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return flag;
    }
    bool flag = false;
};
} // namespace

TEST(Clock, SleepAdvancesAndSucceeds)
{
    FakeClock c;
    FlagToken t;
    Status s = c.sleep(10, t);
    EXPECT_TRUE(s.has_value());
    EXPECT_EQ(c.now_ms(), 10u);
}

TEST(Clock, SleepReturnsCancelledWhenTokenSet)
{
    FakeClock c;
    FlagToken t;
    t.flag = true;
    Status s = c.sleep(10, t);
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().kind, ErrorKind::Cancelled);
}
