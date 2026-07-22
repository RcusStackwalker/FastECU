#include <QtTest>
#include <QApplication>
#include <cstdint>
#include "src/backend/definitions/file_actions.h"
#include "src/backend/logging/protocols/ssm_logging_protocol.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/result.h"
#include "scripted_ssm_transport.h"
#include "test_ssm_logging_protocol.h"

namespace
{
// Deterministic clock for readFramedResponse's deadline loops: now_ms()
// advances by a fixed step on every call, so `elapsed < timeoutMs` loops
// are guaranteed to terminate within a bounded number of iterations instead
// of spinning forever (a constant now_ms() would hang the car-silent path).
class FakeClock : public fastecu::IClock
{
  public:
    std::uint64_t now_ms() const override
    {
        const std::uint64_t value = now_;
        now_ += kStepMs;
        return value;
    }
    fastecu::Status sleep(int, const fastecu::ICancellationToken&) override
    {
        now_ += kStepMs;
        return {};
    }

  private:
    static constexpr std::uint64_t kStepMs = 10;
    mutable std::uint64_t now_ = 0;
};

FileActions::LogValuesStructure makeOneChannel()
{
    FileActions::LogValuesStructure lv;
    lv.lower_panel_log_value_id << "P1";
    lv.log_value_id << "P1";
    lv.log_value_protocol << "SSM";
    lv.log_value_address << "1000";
    lv.log_value_units << "raw,unit,x,0";
    lv.log_value_length << "1";
    lv.log_value_enabled << "1";
    lv.log_value << "0";
    return lv;
}

FileActions::LogValuesStructure makeTwoSelectedChannelsSecondDisabled()
{
    FileActions::LogValuesStructure lv;
    lv.lower_panel_log_value_id << "P1" << "P2";
    lv.log_value_id << "P1" << "P2";
    lv.log_value_protocol << "SSM" << "SSM";
    lv.log_value_address << "1000" << "1003";
    lv.log_value_units << "raw,unit,x,0" << "raw,unit,x,0";
    lv.log_value_length << "1" << "1";
    lv.log_value_enabled << "1" << "0";
    lv.log_value << "0" << "0";
    return lv;
}

bytes::Bytes buildRequest(bytes::ByteView payload)
{
    return SsmProtocol::addHeader(payload, 0xF0, 0x10);
}

bytes::Bytes buildResponse(bytes::ByteView payload)
{
    // [0x80][0xf0][0x10][len][0xe8][payload...][checksum]
    bytes::Bytes msg = {0x80, 0xf0, 0x10, static_cast<bytes::Byte>(payload.size() + 1), 0xe8};
    msg.insert(msg.end(), payload.begin(), payload.end());
    msg.push_back(SsmProtocol::checksum(msg));
    return msg;
}
} // namespace

class TestSsmLoggingProtocol : public QObject
{
    Q_OBJECT
  private slots:
    void start_succeeds_on_well_formed_response()
    {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
        transport->queueRead(buildResponse(bytes::Bytes{0, 0, 0}));
        ScriptedSsmTransport *raw = transport.get();
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();
        FakeClock clock;

        SsmLoggingProtocol proto(clock, std::move(transport), &lv, &fileActions, "SSM", true, false);
        QVERIFY(proto.start().has_value());
        QVERIFY(raw->scriptConsumed());
        QVERIFY(raw->ok());
    }

    void start_fails_on_short_response()
    {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->queueRead(bytes::Bytes{});
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();
        FakeClock clock;

        SsmLoggingProtocol proto(clock, std::move(transport), &lv, &fileActions, "SSM", true, false);
        auto s = proto.start();
        QVERIFY(!s.has_value());
        QCOMPARE((int)s.error().kind, (int)fastecu::ErrorKind::BadResponse);
        QVERIFY(!s.error().detail.empty());
    }

    void start_fails_when_adapter_is_closed()
    {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->setOpen(false);
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();
        FakeClock clock;

        SsmLoggingProtocol proto(clock, std::move(transport), &lv, &fileActions, "SSM", true, false);
        auto s = proto.start();
        QVERIFY(!s.has_value());
        QCOMPARE((int)s.error().kind, (int)fastecu::ErrorKind::Disconnected);
        QCOMPARE(QString::fromStdString(s.error().detail), QString("adapter disconnected"));
    }

    void poll_decodes_one_channel()
    {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
        transport->queueRead(buildResponse(bytes::Bytes{42}));
        ScriptedSsmTransport *raw = transport.get();
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();
        FakeClock clock;

        SsmLoggingProtocol proto(clock, std::move(transport), &lv, &fileActions, "SSM", true, false);
        auto r = proto.poll(200);

        QVERIFY(r.has_value());
        QVERIFY(r->responded);
        QCOMPARE(r->samples.size(), 1);
        QCOMPARE(r->samples.at(0).logValueIndex, 0);
        QCOMPARE(r->samples.at(0).displayValue, QString("42"));
        QVERIFY(raw->scriptConsumed());
        QVERIFY(raw->ok());
    }

    void poll_requests_disabled_selected_channels_but_does_not_emit_samples()
    {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x03}));
        transport->queueRead(buildResponse(bytes::Bytes{42, 99}));
        ScriptedSsmTransport *raw = transport.get();
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeTwoSelectedChannelsSecondDisabled();
        FakeClock clock;

        SsmLoggingProtocol proto(clock, std::move(transport), &lv, &fileActions, "SSM", true, false);
        auto r = proto.poll(200);

        QVERIFY(r.has_value());
        QVERIFY(r->responded);
        QCOMPARE(r->samples.size(), 1);
        QCOMPARE(r->samples.at(0).logValueIndex, 0);
        QCOMPARE(r->samples.at(0).displayValue, QString("42"));
        QVERIFY(raw->scriptConsumed());
        QVERIFY(raw->ok());
    }

    void poll_returns_no_response_on_timeout()
    {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();
        FakeClock clock;

        SsmLoggingProtocol proto(clock, std::move(transport), &lv, &fileActions, "SSM", true, false);
        auto r = proto.poll(50);

        QVERIFY(r.has_value());
        QVERIFY(!r->responded);
    }

    void poll_returns_car_silent_when_frame_header_never_resyncs()
    {
        // Header-resync loop (readFramedResponse's second while): bytes keep
        // arriving but never start with the 0x80,0xf0,0x10 frame header, so
        // the loop must be bounded by the clock, not by running out of bytes
        // to erase. 64 garbage bytes comfortably outlasts the handful of
        // FakeClock ticks needed to cross timeoutMs.
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->queueRead(bytes::Bytes(64, 0x01));
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();
        FakeClock clock;

        SsmLoggingProtocol proto(clock, std::move(transport), &lv, &fileActions, "SSM", true, false);
        auto r = proto.poll(100);

        QVERIFY(r.has_value());
        QVERIFY(!r->responded);
    }

    void poll_returns_transport_error_when_adapter_closes_mid_session()
    {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
        transport->queueRead(buildResponse(bytes::Bytes{1}));
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();
        ScriptedSsmTransport *raw = transport.get();
        FakeClock clock;

        SsmLoggingProtocol proto(clock, std::move(transport), &lv, &fileActions, "SSM", true, false);
        QVERIFY(proto.start().has_value());

        raw->setOpen(false);
        auto r = proto.poll(50);
        QVERIFY(!r.has_value());
        QCOMPARE((int)r.error().kind, (int)fastecu::ErrorKind::Disconnected);
        QCOMPARE(QString::fromStdString(r.error().detail), QString("adapter disconnected"));
    }
};

int run_test_ssm_logging_protocol(int argc, char **argv)
{
    // FileActions derives from QWidget, which requires a QApplication rather than
    // a plain QCoreApplication to construct (even though we never show a widget).
    QApplication app(argc, argv);
    TestSsmLoggingProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_ssm_logging_protocol.moc"
