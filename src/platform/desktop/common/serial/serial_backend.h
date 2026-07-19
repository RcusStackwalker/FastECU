#ifndef SERIAL_BACKEND_H
#define SERIAL_BACKEND_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <stdint.h>

class QObject;

// Internal seam between the SerialPortActions facade and the two adapter
// backends (SerialPortActionsDirect, RemoteSerialBackend). Consumers never
// see this type. Every method executes on the SerialIoThread — implementations
// may block, but must never pump the application event loop.
//
// Method names/signatures deliberately mirror the facade's public surface so
// the facade forwards 1:1. Getter/setter pairs correspond to the direct
// backend's public config fields.
class SerialBackend
{
  public:
    virtual ~SerialBackend() = default;

    // The QObject identity of the concrete backend, used by the facade to
    // wire LOG_* / stateChanged signal forwarding.
    virtual QObject *qobject() = 0;

    // -- config get/set pairs (44) --------------------------------------
    virtual bool get_serialPortAvailable() = 0;
    virtual bool set_serialPortAvailable(bool value) = 0;
    virtual bool get_setRequestToSend() = 0;
    virtual bool set_setRequestToSend(bool value) = 0;
    virtual bool get_setDataTerminalReady() = 0;
    virtual bool set_setDataTerminalReady(bool value) = 0;
    virtual bool get_add_ssm_header() = 0;
    virtual bool set_add_ssm_header(bool value) = 0;
    virtual bool get_add_iso9141_header() = 0;
    virtual bool set_add_iso9141_header(bool value) = 0;
    virtual bool get_add_iso14230_header() = 0;
    virtual bool set_add_iso14230_header(bool value) = 0;
    virtual bool get_is_iso14230_connection() = 0;
    virtual bool set_is_iso14230_connection(bool value) = 0;
    virtual bool get_is_can_connection() = 0;
    virtual bool set_is_can_connection(bool value) = 0;
    virtual bool get_is_iso15765_connection() = 0;
    virtual bool set_is_iso15765_connection(bool value) = 0;
    virtual bool get_is_29_bit_id() = 0;
    virtual bool set_is_29_bit_id(bool value) = 0;
    virtual bool get_use_openport2_adapter() = 0;
    virtual bool set_use_openport2_adapter(bool value) = 0;

    virtual int get_requestToSendEnabled() = 0;
    virtual bool set_requestToSendEnabled(int value) = 0;
    virtual int get_requestToSendDisabled() = 0;
    virtual bool set_requestToSendDisabled(int value) = 0;
    virtual int get_dataTerminalEnabled() = 0;
    virtual bool set_dataTerminalEnabled(int value) = 0;
    virtual int get_dataTerminalDisabled() = 0;
    virtual bool set_dataTerminalDisabled(int value) = 0;

    virtual uint8_t get_kline_startbyte() = 0;
    virtual bool set_kline_startbyte(uint8_t value) = 0;
    virtual uint8_t get_kline_tester_id() = 0;
    virtual bool set_kline_tester_id(uint8_t value) = 0;
    virtual uint8_t get_kline_target_id() = 0;
    virtual bool set_kline_target_id(uint8_t value) = 0;
    virtual uint8_t get_serial_port_parity() = 0;
    virtual bool set_serial_port_parity(uint8_t parity) = 0;

    virtual QByteArray get_ssm_receive_header_start() = 0;
    virtual bool set_ssm_receive_header_start(QByteArray value) = 0;

    virtual QStringList get_serial_port_list() = 0;
    virtual bool set_serial_port_list(QStringList value) = 0;

    virtual QString get_openedSerialPort() = 0;
    virtual bool set_openedSerialPort(QString value) = 0;
    virtual QString get_subaru_02_16bit_bootloader_baudrate() = 0;
    virtual bool set_subaru_02_16bit_bootloader_baudrate(QString value) = 0;
    virtual QString get_subaru_04_16bit_bootloader_baudrate() = 0;
    virtual bool set_subaru_04_16bit_bootloader_baudrate(QString value) = 0;
    virtual QString get_subaru_02_32bit_bootloader_baudrate() = 0;
    virtual bool set_subaru_02_32bit_bootloader_baudrate(QString value) = 0;
    virtual QString get_subaru_04_32bit_bootloader_baudrate() = 0;
    virtual bool set_subaru_04_32bit_bootloader_baudrate(QString value) = 0;
    virtual QString get_subaru_05_32bit_bootloader_baudrate() = 0;
    virtual bool set_subaru_05_32bit_bootloader_baudrate(QString value) = 0;
    virtual QString get_subaru_02_16bit_kernel_baudrate() = 0;
    virtual bool set_subaru_02_16bit_kernel_baudrate(QString value) = 0;
    virtual QString get_subaru_04_16bit_kernel_baudrate() = 0;
    virtual bool set_subaru_04_16bit_kernel_baudrate(QString value) = 0;
    virtual QString get_subaru_02_32bit_kernel_baudrate() = 0;
    virtual bool set_subaru_02_32bit_kernel_baudrate(QString value) = 0;
    virtual QString get_subaru_04_32bit_kernel_baudrate() = 0;
    virtual bool set_subaru_04_32bit_kernel_baudrate(QString value) = 0;
    virtual QString get_subaru_05_32bit_kernel_baudrate() = 0;
    virtual bool set_subaru_05_32bit_kernel_baudrate(QString value) = 0;
    virtual QString get_can_speed() = 0;
    virtual bool set_can_speed(QString value) = 0;
    virtual QString get_serial_port_baudrate() = 0;
    virtual bool set_serial_port_baudrate(QString value) = 0;
    virtual QString get_serial_port_linux() = 0;
    virtual bool set_serial_port_linux(QString value) = 0;
    virtual QString get_serial_port_windows() = 0;
    virtual bool set_serial_port_windows(QString value) = 0;
    virtual QString get_serial_port() = 0;
    virtual bool set_serial_port(QString value) = 0;
    virtual QString get_serial_port_prefix() = 0;
    virtual bool set_serial_port_prefix(QString value) = 0;
    virtual QString get_serial_port_prefix_linux() = 0;
    virtual bool set_serial_port_prefix_linux(QString value) = 0;
    virtual QString get_serial_port_prefix_win() = 0;
    virtual bool set_serial_port_prefix_win(QString value) = 0;

    virtual uint32_t get_can_source_address() = 0;
    virtual bool set_can_source_address(uint32_t value) = 0;
    virtual uint32_t get_can_destination_address() = 0;
    virtual bool set_can_destination_address(uint32_t value) = 0;
    virtual uint32_t get_iso15765_source_address() = 0;
    virtual bool set_iso15765_source_address(uint32_t value) = 0;
    virtual uint32_t get_iso15765_destination_address() = 0;
    virtual bool set_iso15765_destination_address(uint32_t value) = 0;

    // -- operations ------------------------------------------------------
    virtual bool is_serial_port_open() = 0;
    virtual int change_port_speed(QString portSpeed) = 0;
    virtual bool set_kline_timings(uint32_t parameter, int value) = 0;
    virtual int set_j2534_ioctl(uint32_t parameter, int value) = 0;
    virtual QByteArray five_baud_init(QByteArray output) = 0;
    virtual int fast_init(QByteArray output) = 0;
    virtual int set_lec_lines(int lec1, int lec2) = 0;
    virtual int pulse_lec_1_line(int timeout) = 0;
    virtual int pulse_lec_2_line(int timeout) = 0;
    virtual void reset_connection() = 0;
    virtual QByteArray read_serial_obd_data(uint16_t timeout) = 0;
    virtual QByteArray read_serial_data(uint16_t timeout) = 0;
    virtual QByteArray write_serial_data(QByteArray output) = 0;
    virtual QByteArray write_serial_data_echo_check(QByteArray output) = 0;
    virtual bool get_is_tx_done() = 0;
    virtual int clear_rx_buffer() = 0;
    virtual int clear_tx_buffer() = 0;
    virtual int send_periodic_j2534_data(QByteArray output, int timeout) = 0;
    virtual int stop_periodic_j2534_data() = 0;
    virtual QStringList check_serial_ports() = 0;
    virtual QString open_serial_port() = 0;
    virtual unsigned long read_vbatt() = 0;

    // Remote-only: block until the QtRO source is replicated. No-op for the
    // direct backend.
    virtual void waitForSource()
    {
    }
};

#endif // SERIAL_BACKEND_H
