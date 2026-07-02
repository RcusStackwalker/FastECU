#include <QtTest>
#include <QApplication>
#include <file_actions.h>
#include "logging/protocols/cdbg_logging_protocol.h"
#include "serial_port/serial_port_actions.h"
#include "scripted_can_transport.h"
#include "test_cdbg_logging_protocol.h"

using namespace MitsuColtCanCdbg;

namespace {
FileActions::LogValuesStructure makeOneChannel()
{
    FileActions::LogValuesStructure lv;
    lv.lower_panel_log_value_id << "C1";
    lv.log_value_id << "C1";
    lv.log_value_protocol << "CDBG";
    lv.log_value_address << "804000";
    lv.log_value_units << "raw,unit,x,0";
    lv.log_value_length << "1";
    lv.log_value_enabled << "1";
    lv.log_value << "0";
    return lv;
}
}

class TestCdbgLoggingProtocol : public QObject {
    Q_OBJECT
private slots:
    void poll_returns_no_response_before_start() {
        auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
        FileActions::LogValuesStructure lv = makeOneChannel();
        FileActions fileActions;
        SerialPortActions serial;
        CdbgLoggingProtocol proto(std::move(transport), &serial, &lv, &fileActions);

        PollResult r = proto.poll(20);
        QCOMPARE((int)r.status, (int)PollResult::Status::NoResponse);
    }

    void poll_returns_transport_error_when_adapter_closed() {
        auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
        transport->setOpen(false);
        FileActions::LogValuesStructure lv = makeOneChannel();
        FileActions fileActions;
        SerialPortActions serial;
        CdbgLoggingProtocol proto(std::move(transport), &serial, &lv, &fileActions);

        PollResult r = proto.poll(20);
        QCOMPARE((int)r.status, (int)PollResult::Status::TransportError);
    }
};

int run_test_cdbg_logging_protocol(int argc, char** argv) {
    // FileActions derives from QWidget, which requires a QApplication rather than
    // a plain QCoreApplication to construct (even though we never show a widget).
    QApplication app(argc, argv);
    TestCdbgLoggingProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_cdbg_logging_protocol.moc"
