#include <QtTest>
#include <cstdio>
#include <QApplication>
#include <file_actions.h>
#include "logging/protocols/mut_dma_logging_protocol.h"
#include "protocol/mut_dma_codec.h"
#include "scripted_kline_transport.h"
#include "test_mut_dma_logging_protocol.h"

using namespace mutdma;

namespace
{
FileActions::LogValuesStructure makeOneChannel()
{
    FileActions::LogValuesStructure lv;
    lv.lower_panel_log_value_id << "M1";
    lv.log_value_id << "M1";
    lv.log_value_protocol << "MUT_DMA";
    lv.log_value_address << "8000";
    lv.log_value_units << "raw,unit,x,0";
    lv.log_value_length << "2";
    lv.log_value_enabled << "1";
    lv.log_value << "0";
    return lv;
}
} // namespace

class TestMutDmaLoggingProtocol : public QObject
{
    Q_OBJECT
  private slots:
    void start_reaches_streaming_on_valid_handshake()
    {
        auto transport = std::make_unique<ScriptedKlineTransport>();
        FileActions::LogValuesStructure lv = makeOneChannel();
        QVector<Channel> ch = {{0x8000, 2}};
        transport->expectWrite(buildSetupFrame(0xA0, 1));
        transport->queueRead(buildCommandFrame(0xA5, bytes::Bytes{}, TRAILER_STD));
        transport->expectWrite(buildIdListFrame(0xA1, ch));
        transport->queueRead(buildCommandFrame(0x05, bytes::Bytes{}, TRAILER_STD));

        FileActions fileActions;
        MutDmaLoggingProtocol proto(std::move(transport), std::make_unique<AlreadyInMode>(125000), &lv, &fileActions);
        QString err;
        QVERIFY(proto.start(&err));
    }

    void start_fails_when_adapter_is_closed()
    {
        auto transport = std::make_unique<ScriptedKlineTransport>();
        transport->setOpen(false);
        FileActions::LogValuesStructure lv = makeOneChannel();
        FileActions fileActions;
        MutDmaLoggingProtocol proto(std::move(transport), std::make_unique<AlreadyInMode>(125000), &lv, &fileActions);

        QString err;
        QVERIFY(!proto.start(&err));
        QCOMPARE(err, QString("adapter disconnected"));
    }

    void poll_returns_no_response_before_start()
    {
        auto transport = std::make_unique<ScriptedKlineTransport>();
        FileActions::LogValuesStructure lv = makeOneChannel();
        FileActions fileActions;
        MutDmaLoggingProtocol proto(std::move(transport), std::make_unique<AlreadyInMode>(125000), &lv, &fileActions);

        PollResult r = proto.poll(20);
        QCOMPARE((int)r.status, (int)PollResult::Status::NoResponse);
    }

    void poll_returns_transport_error_when_adapter_closed()
    {
        auto transport = std::make_unique<ScriptedKlineTransport>();
        transport->setOpen(false);
        FileActions::LogValuesStructure lv = makeOneChannel();
        FileActions fileActions;
        MutDmaLoggingProtocol proto(std::move(transport), std::make_unique<AlreadyInMode>(125000), &lv, &fileActions);

        PollResult r = proto.poll(20);
        QCOMPARE((int)r.status, (int)PollResult::Status::TransportError);
    }

    void poll_returns_transport_error_when_adapter_closes_mid_session()
    {
        auto transport = std::make_unique<ScriptedKlineTransport>();
        FileActions::LogValuesStructure lv = makeOneChannel();
        QVector<Channel> ch = {{0x8000, 2}};
        transport->expectWrite(buildSetupFrame(0xA0, 1));
        transport->queueRead(buildCommandFrame(0xA5, bytes::Bytes{}, TRAILER_STD));
        transport->expectWrite(buildIdListFrame(0xA1, ch));
        transport->queueRead(buildCommandFrame(0x05, bytes::Bytes{}, TRAILER_STD));
        ScriptedKlineTransport *raw = transport.get();

        FileActions fileActions;
        MutDmaLoggingProtocol proto(std::move(transport), std::make_unique<AlreadyInMode>(125000), &lv, &fileActions);
        QString err;
        QVERIFY(proto.start(&err));

        raw->setOpen(false);
        PollResult r = proto.poll(20);
        QCOMPARE((int)r.status, (int)PollResult::Status::TransportError);
    }
};

int run_test_mut_dma_logging_protocol(int argc, char **argv)
{
    // FileActions derives from QWidget, which requires a QApplication rather than
    // a plain QCoreApplication to construct (even though we never show a widget).
    fprintf(stderr, "[diag] before QApplication construction\n");
    fflush(stderr);
    QApplication app(argc, argv);
    fprintf(stderr, "[diag] after QApplication construction\n");
    fflush(stderr);
    TestMutDmaLoggingProtocol t;
    fprintf(stderr, "[diag] before QTest::qExec\n");
    fflush(stderr);
    return QTest::qExec(&t, argc, argv);
}
#include "test_mut_dma_logging_protocol.moc"
