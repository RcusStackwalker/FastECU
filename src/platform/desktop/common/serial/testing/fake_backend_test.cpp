#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <gtest/gtest.h>

#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

TEST(FakeBackendTest, defaultActionsPreserveConfigurationThroughFacade)
{
    SerialPortActions serial([]() -> SerialBackend * { return new NiceFakeBackend; });
    ASSERT_TRUE(serial.SetAddIso14230Header(true));
    ASSERT_TRUE(serial.SetCanSourceAddress(0x7E1));
    ASSERT_TRUE(serial.SetSerialPortBaudrate("10400"));
    ASSERT_EQ(serial.GetAddIso14230Header(), true);
    ASSERT_EQ(serial.GetCanSourceAddress(), std::uint32_t{0x7E1});
    ASSERT_EQ(serial.GetSerialPortBaudrate(), QStringLiteral("10400"));
    // Hardware operations have inert defaults, even without a selected port.
    ASSERT_EQ(serial.OpenSerialPort(), QString{});
    ASSERT_EQ(serial.ReadSerialData(10), QByteArray{});
}

TEST(FakeBackendTest, expectationsScriptFacadeIoInOrder)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend;
            return fake;
        });
    ASSERT_TRUE(serial.SetAddSsmHeader(false)); // create backend before expectations
    ::testing::InSequence sequence;
    EXPECT_CALL(*fake, WriteSerialData(QByteArray("request"))).WillOnce(::testing::Return(QByteArray("request")));
    EXPECT_CALL(*fake, ReadSerialData(50)).WillOnce(::testing::Return(QByteArray("reply")));
    ASSERT_EQ(serial.WriteSerialData("request"), QByteArray("request"));
    ASSERT_EQ(serial.ReadSerialData(50), QByteArray("reply"));
}

TEST(FakeBackendTest, expectationFailuresProduceNonzeroExit)
{
    const QString mode = qEnvironmentVariable("FASTECU_GMOCK_FAILURE_PROBE");
    if (!mode.isEmpty())
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial(
            [&fake]() -> SerialBackend *
            {
                fake = new NiceFakeBackend;
                return fake;
            });
        ASSERT_TRUE(serial.SetAddSsmHeader(false));
        if (mode == "unmet")
        {
            EXPECT_CALL(*fake, ReadSerialData(50)).Times(1);
        }
        else if (mode == "forbidden")
        {
            EXPECT_CALL(*fake, ReadSerialData(::testing::_)).Times(0);
            serial.ReadSerialData(50);
        }
        else
        {
            EXPECT_CALL(*fake, ReadSerialData(50)).Times(1);
            serial.ReadSerialData(60);
        }
        return; // mock destruction verifies expectations on the facade's I/O thread
    }

    for (const QString& probe :
         {QStringLiteral("unmet"), QStringLiteral("forbidden"), QStringLiteral("wrong-argument")})
    {
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("FASTECU_GMOCK_FAILURE_PROBE", probe);
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(),
                    {"--gtest_filter=FakeBackendTest.expectationFailuresProduceNonzeroExit"});
        ASSERT_TRUE(child.waitForStarted(5000)) << qPrintable(child.errorString());
        ASSERT_TRUE(child.waitForFinished(10000)) << qPrintable(child.errorString());
        ASSERT_EQ(child.exitStatus(), QProcess::NormalExit);
        ASSERT_EQ(child.exitCode(), 1);
        // GoogleTest labels a failed assertion "Failure", except under
        // MSVC, where it emits Visual Studio's "error: " instead
        // (TestPartResultTypeToString in googletest/src/gtest.cc). Matching
        // only "Failure" therefore never matches on Windows. Accept either
        // spelling, and pin the check to the mocked call the diagnostic has
        // to name, so this still proves GMock reported *this* expectation
        // rather than that some incidental text was printed.
        const QByteArray diagnostics = child.readAll();
        ASSERT_TRUE(diagnostics.contains("Failure") || diagnostics.contains("error: ")) << diagnostics.constData();
        ASSERT_TRUE(diagnostics.contains("ReadSerialData")) << diagnostics.constData();
    }
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
