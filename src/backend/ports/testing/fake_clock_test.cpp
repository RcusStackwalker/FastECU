#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/cancellation.h"
#include <gtest/gtest.h>

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::ICancellationToken;
using fastecu::IClock;
using fastecu::Status;

namespace
{

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
