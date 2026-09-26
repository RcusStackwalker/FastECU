#include <QStringList>
#include <QTest>

#include "src/platform/desktop/common/serial/desktop_serial_factory.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

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
    void buildsADirectFacadeWhenNoPeerIsGiven()
    {
        RecordingLogSink sink;
        const OwnedSerialPortActions serial = make_serial_port_actions({}, {}, sink);
        QVERIFY(serial != nullptr);
        QVERIFY(serial->isDirectConnection());
    }

    void everyLogLevelReachesTheSink()
    {
        RecordingLogSink sink;
        const OwnedSerialPortActions serial = make_serial_port_actions({}, {}, sink);
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
