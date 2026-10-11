#include "src/platform/desktop/common/testing/core_application_environment.h"
// Unit tests for the FakeBackedSerial fixture itself. The 60-odd call sites
// in desktop_can_flash_transport_test.cpp and
// desktop_kline_flash_transport_test.cpp exercise it indirectly; these three
// cases pin the properties those sites rely on but never assert.
#include "src/platform/desktop/common/serial/testing/fake_backed_serial.h"

#include <QCoreApplication>
#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <memory>

// The facade creates its backend lazily, on the first marshaled call. The
// fixture makes that call itself, so fake() is usable with no forcing
// call and no null check at the test site.
TEST(TestFakeBackedSerial, theBackendIsLiveAsSoonAsTheFixtureIsConstructed)
{
    FakeBackedSerial serial;

    EXPECT_CALL(serial.Fake(), ReadVbatt()).WillOnce(::testing::Return(11676UL));

    ASSERT_EQ(serial->ReadVbatt(), 11676UL);
}

// arrange() must run before the fixture's own forcing call reaches the
// backend. Under a StrictMock that call is a test failure unless the
// expectation is already in place, which is what makes this observable.
TEST(TestFakeBackedSerial, arrangeRunsBeforeTheFixtureTouchesTheBackend)
{
    FakeBackedSerial<::testing::StrictMock<FakeBackend>> serial{
        [](auto& fake)
        {
            EXPECT_CALL(fake, SetAddSsmHeader(false)).WillOnce(::testing::DoDefault());
            EXPECT_CALL(fake, ReadVbatt()).WillOnce(::testing::Return(9000UL));
        }};

    ASSERT_EQ(serial->ReadVbatt(), 9000UL);
}

// release() hands the facade to a consumer (a transport, in the real
// suites) while fake() keeps answering, and the backend dies with whoever
// took the facade rather than with the fixture.
TEST(TestFakeBackedSerial, releaseTransfersTheFacadeAndLeavesTheFakeReachable)
{
    bool destroyed = false;
    {
        FakeBackedSerial serial{[&destroyed](auto& fake) { fake.destroyed = &destroyed; }};
        std::unique_ptr<SerialPortActions> owned = serial.Release();
        ASSERT_TRUE(owned != nullptr);
        ASSERT_TRUE(!destroyed);

        EXPECT_CALL(serial.Fake(), ReadVbatt()).WillOnce(::testing::Return(7UL));
        ASSERT_EQ(owned->ReadVbatt(), 7UL);
    }
    ASSERT_TRUE(destroyed);
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
