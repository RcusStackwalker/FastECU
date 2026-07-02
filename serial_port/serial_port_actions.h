#ifndef SERIAL_PORT_ACTIONS_H
#define SERIAL_PORT_ACTIONS_H

#include <QCoreApplication>
#include <QMutex>
#include <QObject>
#include <QSemaphore>
#include <QThread>
#include <QtRemoteObjects/qremoteobjectnode.h>
#include <atomic>
#include <functional>
#include <memory>
#include <type_traits>

// Kept for consumers that relied on this header's transitive includes
// (QSerialPort types, WebSocketIoDevice). Candidates for removal in spec 1b.
#include "serial_port_actions_direct.h"
#include "websocketiodevice.h"

#include "serial_backend.h"

class SerialBackendHost;

// Thin marshaling facade over a SerialBackend hosted on the SerialIoThread
// (see docs/superpowers/specs/2026-07-02-serial-backend-decoupling-design.md).
// The public surface is unchanged from the pre-refactor class; every method
// forwards to the backend on the I/O thread and blocks the caller until the
// result is ready.
class SerialPortActions : public QObject
{
    Q_OBJECT

signals:
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

public:
    explicit SerialPortActions(QString peerAddress = "",
                               QString password = "",
                               QWebSocket *web_socket = nullptr,
                               QObject *parent = nullptr,
                               std::function<SerialBackend *()> backendFactoryForTests = {});
    ~SerialPortActions();

    bool isDirectConnection(void);

    bool get_serialPortAvailable();
    bool set_serialPortAvailable(bool value);
    bool get_setRequestToSend();
    bool set_setRequestToSend(bool value);
    bool get_setDataTerminalReady();
    bool set_setDataTerminalReady(bool value);

    bool get_add_ssm_header();
    bool set_add_ssm_header(bool value);
    bool get_add_iso9141_header();
    bool set_add_iso9141_header(bool value);
    bool get_add_iso14230_header();
    bool set_add_iso14230_header(bool value);
    bool get_is_iso14230_connection();
    bool set_is_iso14230_connection(bool value);
    bool get_is_can_connection();
    bool set_is_can_connection(bool value);
    bool get_is_iso15765_connection();
    bool set_is_iso15765_connection(bool value);
    bool get_is_29_bit_id();
    bool set_is_29_bit_id(bool value);

    bool get_use_openport2_adapter();
    bool set_use_openport2_adapter(bool value);

    int  get_requestToSendEnabled();
    bool set_requestToSendEnabled(int value);
    int  get_requestToSendDisabled();
    bool set_requestToSendDisabled(int value);
    int  get_dataTerminalEnabled();
    bool set_dataTerminalEnabled(int value);
    int  get_dataTerminalDisabled();
    bool set_dataTerminalDisabled(int value);

    bool get_is_comm_busy();
    void set_comm_busy(bool value);
    bool get_read_vbatt();
    void set_read_vbatt(bool value);

    uint8_t get_kline_startbyte();
    bool    set_kline_startbyte(uint8_t value);
    uint8_t get_kline_tester_id();
    bool    set_kline_tester_id(uint8_t value);
    uint8_t get_kline_target_id();
    bool    set_kline_target_id(uint8_t value);

    QByteArray get_ssm_receive_header_start();
    bool       set_ssm_receive_header_start(QByteArray value);

    QStringList get_serial_port_list();
    bool        set_serial_port_list(QStringList value);
    QString get_openedSerialPort();
    bool    set_openedSerialPort(QString value);
    QString get_subaru_02_16bit_bootloader_baudrate();
    bool    set_subaru_02_16bit_bootloader_baudrate(QString value);
    QString get_subaru_04_16bit_bootloader_baudrate();
    bool    set_subaru_04_16bit_bootloader_baudrate(QString value);
    QString get_subaru_02_32bit_bootloader_baudrate();
    bool    set_subaru_02_32bit_bootloader_baudrate(QString value);
    QString get_subaru_04_32bit_bootloader_baudrate();
    bool    set_subaru_04_32bit_bootloader_baudrate(QString value);
    QString get_subaru_05_32bit_bootloader_baudrate();
    bool    set_subaru_05_32bit_bootloader_baudrate(QString value);

    QString get_subaru_02_16bit_kernel_baudrate();
    bool    set_subaru_02_16bit_kernel_baudrate(QString value);
    QString get_subaru_04_16bit_kernel_baudrate();
    bool    set_subaru_04_16bit_kernel_baudrate(QString value);
    QString get_subaru_02_32bit_kernel_baudrate();
    bool    set_subaru_02_32bit_kernel_baudrate(QString value);
    QString get_subaru_04_32bit_kernel_baudrate();
    bool    set_subaru_04_32bit_kernel_baudrate(QString value);
    QString get_subaru_05_32bit_kernel_baudrate();
    bool    set_subaru_05_32bit_kernel_baudrate(QString value);

    QString get_can_speed();
    bool    set_can_speed(QString value);

    uint8_t get_serial_port_parity();
    bool    set_serial_port_parity(uint8_t parity);
    QString get_serial_port_baudrate();
    bool    set_serial_port_baudrate(QString value);
    QString get_serial_port_linux();
    bool    set_serial_port_linux(QString value);
    QString get_serial_port_windows();
    bool    set_serial_port_windows(QString value);
    QString get_serial_port();
    bool    set_serial_port(QString value);
    QString get_serial_port_prefix();
    bool    set_serial_port_prefix(QString value);
    QString get_serial_port_prefix_linux();
    bool    set_serial_port_prefix_linux(QString value);
    QString get_serial_port_prefix_win();
    bool    set_serial_port_prefix_win(QString value);

    uint32_t get_can_source_address();
    bool     set_can_source_address(uint32_t value);
    uint32_t get_can_destination_address();
    bool     set_can_destination_address(uint32_t value);
    uint32_t get_iso15765_source_address();
    bool     set_iso15765_source_address(uint32_t value);
    uint32_t get_iso15765_destination_address();
    bool     set_iso15765_destination_address(uint32_t value);

    bool     set_kline_timings(uint32_t parameter, int value);

    int set_j2534_ioctl(uint32_t parameter, int value);

    bool is_serial_port_open(void);
    int change_port_speed(QString portSpeed);
    QByteArray five_baud_init(QByteArray output);
    int fast_init(QByteArray output);
    int set_lec_lines(int lec1, int lec2);
    int pulse_lec_1_line(int timeout);
    int pulse_lec_2_line(int timeout);
    bool get_is_tx_done();

    bool reset_connection(void);

    QByteArray read_serial_obd_data(uint16_t timeout);
    QByteArray read_serial_data(uint16_t timeout);
    QByteArray write_serial_data(QByteArray output);
    QByteArray write_serial_data_echo_check(QByteArray output);

    int clear_rx_buffer(void);
    int clear_tx_buffer(void);

    int send_periodic_j2534_data(QByteArray output, int timeout);
    int stop_periodic_j2534_data(void);

    QStringList check_serial_ports(void);
    QString open_serial_port(void);

    QString parse_message_to_hex(QByteArray received);

    unsigned long read_vbatt();

public slots:
    void waitForSource(void);

private:
    void ensureBackendStarted();
    void waitForDone(const std::shared_ptr<QSemaphore> &done);

    // Marshal `fn` onto the I/O thread and block until it completes. `fn`
    // runs with m_backend valid and is the ONLY code that touches it.
    template <typename Fn>
    auto runOnBackend(Fn fn)
    {
        using Ret = std::invoke_result_t<Fn &>;
        ensureBackendStarted();
        if (QThread::currentThread() == m_ioThread)
            return fn();   // already on the I/O thread (backend-side callback)
        auto done = std::make_shared<QSemaphore>();
        if constexpr (std::is_void_v<Ret>)
        {
            QMetaObject::invokeMethod(m_ioContext, [fn, done]() mutable
                                      { fn(); done->release(); },
                                      Qt::QueuedConnection);
            waitForDone(done);
        }
        else
        {
            Ret result{};
            QMetaObject::invokeMethod(m_ioContext, [fn, done, &result]() mutable
                                      { result = fn(); done->release(); },
                                      Qt::QueuedConnection);
            waitForDone(done);
            return result;
        }
    }

    QString peerAddress;
    QString password;
    QWebSocket *externalSocket = nullptr;
    std::function<SerialBackend *()> backendFactory;

    QMutex startMutex;
    SerialBackendHost *m_host = nullptr;
    SerialBackend *m_backend = nullptr;
    QObject *m_ioContext = nullptr;   // == m_host->context(), cached
    QThread *m_ioThread = nullptr;    // == m_host->ioThread(), cached

    QAtomicInteger<bool> is_read_vbatt = false;
    QAtomicInteger<bool> is_comm_busy = false;
    std::atomic<unsigned long> vBatt{0};
};

#endif // SERIAL_PORT_ACTIONS_H
