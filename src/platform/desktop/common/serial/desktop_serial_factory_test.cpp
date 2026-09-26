#include <QStringList>
#include <QTest>

#include <memory>

#include "src/platform/desktop/common/serial/desktop_serial_factory.h"
#include "src/platform/desktop/common/serial/remote_serial_backend.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

class RecordingLogSink : public QObject
{
    Q_OBJECT

  public:
    QStringList messages;

  public slots:
    void log_messages(const QString& message, bool /*timestamp*/, bool /*linefeed*/)
    {
        messages << message;
    }
};

class DesktopSerialFactoryTest : public QObject
{
    Q_OBJECT

  private slots:
    void directConnectionBuildsTheDirectBackend()
    {
        const auto factory = make_serial_backend_factory(DirectSerial{});
        QVERIFY(factory);
        const std::unique_ptr<SerialBackend> backend{factory()};
        QVERIFY(dynamic_cast<SerialPortActionsDirect *>(backend.get()) != nullptr);
    }

    // An unreachable peer: RemoteSerialBackend's constructor does not block
    // on it (see remote_backend_smoke_test.cpp).
    void remoteConnectionBuildsTheRemoteBackend()
    {
        const auto factory = make_serial_backend_factory(RemoteSerial{"local:fastecu-test-nonexistent", "pw"});
        QVERIFY(factory);
        const std::unique_ptr<SerialBackend> backend{factory()};
        QVERIFY(dynamic_cast<RemoteSerialBackend *>(backend.get()) != nullptr);
    }

    void everyLogLevelReachesTheSink()
    {
        RecordingLogSink sink;
        const OwnedSerialPortActions serial = make_serial_port_actions(DirectSerial{}, sink);
        QVERIFY(serial != nullptr);
        sink.messages.clear();

        emit serial->LOG_E("error", false, false);
        emit serial->LOG_W("warning", false, false);
        emit serial->LOG_I("info", false, false);
        emit serial->LOG_D("debug", false, false);

        QCOMPARE(sink.messages, (QStringList{"error", "warning", "info", "debug"}));
    }
};

QTEST_GUILESS_MAIN(DesktopSerialFactoryTest)
#include "desktop_serial_factory_test.moc"
