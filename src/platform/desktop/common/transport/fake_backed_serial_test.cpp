// Unit tests for the FakeBackedSerial fixture itself. The 60-odd call sites
// in desktop_can_flash_transport_test.cpp and
// desktop_kline_flash_transport_test.cpp exercise it indirectly; these three
// cases pin the properties those sites rely on but never assert.
#include "src/platform/desktop/common/transport/fake_backed_serial.h"

#include <QCoreApplication>
#include <QTest>

#include <gmock/gmock.h>

#include <memory>

class TestFakeBackedSerial : public QObject
{
    Q_OBJECT

  private slots:

    // The facade creates its backend lazily, on the first marshaled call. The
    // fixture makes that call itself, so fake() is usable with no forcing
    // call and no null check at the test site.
    void theBackendIsLiveAsSoonAsTheFixtureIsConstructed()
    {
        FakeBackedSerial serial;

        EXPECT_CALL(serial.fake(), read_vbatt()).WillOnce(::testing::Return(11676UL));

        QCOMPARE(serial->read_vbatt(), 11676UL);
    }

    // arrange() must run before the fixture's own forcing call reaches the
    // backend. Under a StrictMock that call is a test failure unless the
    // expectation is already in place, which is what makes this observable.
    void arrangeRunsBeforeTheFixtureTouchesTheBackend()
    {
        FakeBackedSerial<::testing::StrictMock<FakeBackend>> serial{
            [](auto& fake)
            {
                EXPECT_CALL(fake, set_add_ssm_header(false)).WillOnce(::testing::DoDefault());
                EXPECT_CALL(fake, read_vbatt()).WillOnce(::testing::Return(9000UL));
            }};

        QCOMPARE(serial->read_vbatt(), 9000UL);
    }

    // release() hands the facade to a consumer (a transport, in the real
    // suites) while fake() keeps answering, and the backend dies with whoever
    // took the facade rather than with the fixture.
    void releaseTransfersTheFacadeAndLeavesTheFakeReachable()
    {
        bool destroyed = false;
        {
            FakeBackedSerial serial{[&destroyed](auto& fake) { fake.destroyed = &destroyed; }};
            std::unique_ptr<SerialPortActions> owned = serial.release();
            QVERIFY(owned != nullptr);
            QVERIFY(!destroyed);

            EXPECT_CALL(serial.fake(), read_vbatt()).WillOnce(::testing::Return(7UL));
            QCOMPARE(owned->read_vbatt(), 7UL);
        }
        QVERIFY(destroyed);
    }
};

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QCoreApplication application(argc, argv);
    TestFakeBackedSerial test;
    const int result = QTest::qExec(&test, argc, argv);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "fake_backed_serial_test.moc"
