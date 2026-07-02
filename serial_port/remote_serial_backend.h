#ifndef REMOTE_SERIAL_BACKEND_H
#define REMOTE_SERIAL_BACKEND_H

#include <QObject>
#include <QtRemoteObjects/qremoteobjectnode.h>

#include "serial_backend.h"
#include "websocketiodevice.h"

class SerialPortActionsRemoteReplica;

// SerialBackend implementation for the remote (QtRemoteObjects over
// WebSocket, or local socket) adapter path. Strictly a mechanical wrap of
// the replica calls that used to live inline in SerialPortActions — no wire
// behavior change.
//
// Must be constructed ON the thread that will run it (SerialIoThread): the
// node, websocket, and replica all take that thread's affinity.
//
// Blocking-wait caveat, accepted by the spec: qtrohelper::slot_sync() runs a
// local event loop while waiting for the reply, so — exactly like today's
// GUI-thread behavior — other queued backend calls can interleave on the
// remote path. The strict no-interleave guarantee applies to the direct
// backend only.
class RemoteSerialBackend : public QObject, public SerialBackend
{
    Q_OBJECT

signals:
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

public:
    explicit RemoteSerialBackend(QString peerAddress,
                                 QString password,
                                 QWebSocket *externalSocket = nullptr,
                                 QObject *parent = nullptr);
    ~RemoteSerialBackend() override;

    QObject *qobject() override { return this; }
    void waitForSource() override;

    // -- config get/set pairs (44) --------------------------------------
    bool get_serialPortAvailable() override;
    bool set_serialPortAvailable(bool value) override;
    bool get_setRequestToSend() override;
    bool set_setRequestToSend(bool value) override;
    bool get_setDataTerminalReady() override;
    bool set_setDataTerminalReady(bool value) override;
    bool get_add_ssm_header() override;
    bool set_add_ssm_header(bool value) override;
    bool get_add_iso9141_header() override;
    bool set_add_iso9141_header(bool value) override;
    bool get_add_iso14230_header() override;
    bool set_add_iso14230_header(bool value) override;
    bool get_is_iso14230_connection() override;
    bool set_is_iso14230_connection(bool value) override;
    bool get_is_can_connection() override;
    bool set_is_can_connection(bool value) override;
    bool get_is_iso15765_connection() override;
    bool set_is_iso15765_connection(bool value) override;
    bool get_is_29_bit_id() override;
    bool set_is_29_bit_id(bool value) override;
    bool get_use_openport2_adapter() override;
    bool set_use_openport2_adapter(bool value) override;

    int  get_requestToSendEnabled() override;
    bool set_requestToSendEnabled(int value) override;
    int  get_requestToSendDisabled() override;
    bool set_requestToSendDisabled(int value) override;
    int  get_dataTerminalEnabled() override;
    bool set_dataTerminalEnabled(int value) override;
    int  get_dataTerminalDisabled() override;
    bool set_dataTerminalDisabled(int value) override;

    uint8_t get_kline_startbyte() override;
    bool    set_kline_startbyte(uint8_t value) override;
    uint8_t get_kline_tester_id() override;
    bool    set_kline_tester_id(uint8_t value) override;
    uint8_t get_kline_target_id() override;
    bool    set_kline_target_id(uint8_t value) override;
    uint8_t get_serial_port_parity() override;
    bool    set_serial_port_parity(uint8_t parity) override;

    QByteArray get_ssm_receive_header_start() override;
    bool       set_ssm_receive_header_start(QByteArray value) override;

    QStringList get_serial_port_list() override;
    bool        set_serial_port_list(QStringList value) override;

    QString get_openedSerialPort() override;
    bool    set_openedSerialPort(QString value) override;
    QString get_subaru_02_16bit_bootloader_baudrate() override;
    bool    set_subaru_02_16bit_bootloader_baudrate(QString value) override;
    QString get_subaru_04_16bit_bootloader_baudrate() override;
    bool    set_subaru_04_16bit_bootloader_baudrate(QString value) override;
    QString get_subaru_02_32bit_bootloader_baudrate() override;
    bool    set_subaru_02_32bit_bootloader_baudrate(QString value) override;
    QString get_subaru_04_32bit_bootloader_baudrate() override;
    bool    set_subaru_04_32bit_bootloader_baudrate(QString value) override;
    QString get_subaru_05_32bit_bootloader_baudrate() override;
    bool    set_subaru_05_32bit_bootloader_baudrate(QString value) override;
    QString get_subaru_02_16bit_kernel_baudrate() override;
    bool    set_subaru_02_16bit_kernel_baudrate(QString value) override;
    QString get_subaru_04_16bit_kernel_baudrate() override;
    bool    set_subaru_04_16bit_kernel_baudrate(QString value) override;
    QString get_subaru_02_32bit_kernel_baudrate() override;
    bool    set_subaru_02_32bit_kernel_baudrate(QString value) override;
    QString get_subaru_04_32bit_kernel_baudrate() override;
    bool    set_subaru_04_32bit_kernel_baudrate(QString value) override;
    QString get_subaru_05_32bit_kernel_baudrate() override;
    bool    set_subaru_05_32bit_kernel_baudrate(QString value) override;
    QString get_can_speed() override;
    bool    set_can_speed(QString value) override;
    QString get_serial_port_baudrate() override;
    bool    set_serial_port_baudrate(QString value) override;
    QString get_serial_port_linux() override;
    bool    set_serial_port_linux(QString value) override;
    QString get_serial_port_windows() override;
    bool    set_serial_port_windows(QString value) override;
    QString get_serial_port() override;
    bool    set_serial_port(QString value) override;
    QString get_serial_port_prefix() override;
    bool    set_serial_port_prefix(QString value) override;
    QString get_serial_port_prefix_linux() override;
    bool    set_serial_port_prefix_linux(QString value) override;
    QString get_serial_port_prefix_win() override;
    bool    set_serial_port_prefix_win(QString value) override;

    uint32_t get_can_source_address() override;
    bool     set_can_source_address(uint32_t value) override;
    uint32_t get_can_destination_address() override;
    bool     set_can_destination_address(uint32_t value) override;
    uint32_t get_iso15765_source_address() override;
    bool     set_iso15765_source_address(uint32_t value) override;
    uint32_t get_iso15765_destination_address() override;
    bool     set_iso15765_destination_address(uint32_t value) override;

    // -- operations ------------------------------------------------------
    bool is_serial_port_open() override;
    int change_port_speed(QString portSpeed) override;
    bool set_kline_timings(uint32_t parameter, int value) override;
    int set_j2534_ioctl(uint32_t parameter, int value) override;
    QByteArray five_baud_init(QByteArray output) override;
    int fast_init(QByteArray output) override;
    int set_lec_lines(int lec1, int lec2) override;
    int pulse_lec_1_line(int timeout) override;
    int pulse_lec_2_line(int timeout) override;
    void reset_connection() override;
    QByteArray read_serial_obd_data(uint16_t timeout) override;
    QByteArray read_serial_data(uint16_t timeout) override;
    QByteArray write_serial_data(QByteArray output) override;
    QByteArray write_serial_data_echo_check(QByteArray output) override;
    bool get_is_tx_done() override;
    int clear_rx_buffer() override;
    int clear_tx_buffer() override;
    int send_periodic_j2534_data(QByteArray output, int timeout) override;
    int stop_periodic_j2534_data() override;
    QStringList check_serial_ports() override;
    QString open_serial_port() override;
    unsigned long read_vbatt() override;

private:
    QString peerAddress;
    QString password;

    const QString autodiscoveryMessage = "FastECU_PTP_Autodiscovery";
    const QString remoteObjectName = "FastECU";
    const QString wssPath = "/" + remoteObjectName;
    const QString webSocketPasswordHeader = "fastecu-basic-password";
    const int heartbeatInterval = 0;

    QWebSocket *webSocket = nullptr;
    WebSocketIoDevice *socket = nullptr;
    QRemoteObjectNode node;
    SerialPortActionsRemoteReplica *serial_remote = nullptr;

    void startRemote();
    void startOverNetwork();
    void startLocal();
    void sendAutoDiscoveryMessage();

private slots:
    void websocket_connected();
    void serialRemoteStateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);
};

#endif // REMOTE_SERIAL_BACKEND_H
