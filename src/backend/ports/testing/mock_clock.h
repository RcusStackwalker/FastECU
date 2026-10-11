#pragma once
#include "src/backend/ports/clock.h"
#include "src/backend/ports/testing/fake_clock.h"

#include <chrono>

#include <gmock/gmock.h>

namespace fastecu
{

// A gmock IClock for tests that care how the code under test sleeps: a sleep
// that must happen (n times), must not happen, fails, or triggers something.
// By default both methods delegate to an embedded FakeClock, so time advances
// and cancellation is honoured unless a test says otherwise. Tests that only
// need time to pass use FakeClock; tests asserting the exact ordered list of
// sleeps use RecordingClock.
//
//   MockClock clock;
//   EXPECT_CALL(clock, sleep(50ms, _)).Times(12);
//   EXPECT_CALL(clock, sleep(3ms, _))
//       .WillOnce(DoAll([&] { token.set_cancelled(true); }, clock.sleep_on_fake()));
//
// The constructor adds catch-all AnyNumber() expectations. Without them, one
// EXPECT_CALL(clock, sleep(3ms, _)) would make every *other* sleep an
// "unexpected call" -- NiceMock only forgives methods with no expectation at
// all. A test's own expectations are newer and take precedence; to forbid
// unmentioned sleeps, add EXPECT_CALL(clock, sleep).Times(0) before them.
class MockClock : public IClock
{
  public:
    MockClock()
    {
        ON_CALL(*this, Now()).WillByDefault([this] { return fake_.Now(); });
        ON_CALL(*this, Sleep).WillByDefault(SleepOnFake());
        EXPECT_CALL(*this, Now()).Times(::testing::AnyNumber());
        EXPECT_CALL(*this, Sleep).Times(::testing::AnyNumber());
    }

    MOCK_METHOD(std::chrono::steady_clock::time_point, Now, (), (const, override));
    MOCK_METHOD(Status, Sleep, (std::chrono::milliseconds, const ICancellationToken&), (override));

    // The default sleep as an action. DoDefault() cannot appear inside
    // DoAll(), so compose with this instead; DoAll returns its last action's
    // result, so put it last.
    ::testing::Action<Status(std::chrono::milliseconds, const ICancellationToken&)> SleepOnFake()
    {
        return [this](std::chrono::milliseconds duration, const ICancellationToken& cancellation)
        { return fake_.Sleep(duration, cancellation); };
    }

    std::chrono::milliseconds Elapsed() const
    {
        return fake_.Elapsed();
    }

    // The embedded time model, for an action that must act *after* the real
    // sleep and still return its result -- which DoAll(), returning its last
    // action's value, cannot express.
    FakeClock& Fake()
    {
        return fake_;
    }

  private:
    FakeClock fake_;
};

} // namespace fastecu
