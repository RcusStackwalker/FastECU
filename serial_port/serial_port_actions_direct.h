#ifndef SERIAL_PORT_ACTIONS_DIRECT_H
#define SERIAL_PORT_ACTIONS_DIRECT_H

#include <QCoreApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDebug>
#include <QElapsedTimer>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QDateTime>
#include <QTime>
#include <QTimer>
#include <QWidget>
#include <QSettings>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <thread>
#include <chrono>

#if defined Q_OS_UNIX
#include "J2534_unix.h"
#elif defined Q_OS_WIN32
#include "J2534_win.h"
#endif

#include "serial_backend.h"

class SerialPortActionsDirect : public QObject, public SerialBackend
{
    Q_OBJECT

signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

public:
    explicit SerialPortActionsDirect(QObject *parent=nullptr);
    ~SerialPortActionsDirect();

    bool serialPortAvailable = false;
    bool setRequestToSend = true;
    bool setDataTerminalReady = true;
    bool signalToReadBattVoltage = false;

    bool add_ssm_header = false;
    bool add_iso9141_header = false;
    bool add_iso14230_header = false;
    bool is_iso14230_connection = false;
    bool is_can_connection = false;
    bool is_iso15765_connection = false;
    bool is_29_bit_id = false;

    bool use_openport2_adapter = false;

    unsigned long vBatt = 0;

    int requestToSendEnabled = 0;
    int requestToSendDisabled = 1;
    int dataTerminalEnabled = 0;
    int dataTerminalDisabled = 1;

    uint16_t echo_check_timout = 5000;
    uint16_t receive_timeout = 500;
    uint16_t serial_read_timeout = 2000;
    uint16_t serial_read_extra_short_timeout = 50;
    uint16_t serial_read_short_timeout = 200;
    uint16_t serial_read_medium_timeout = 500;
    uint16_t serial_read_long_timeout = 800;
    uint16_t serial_read_extra_long_timeout = 3000;

    uint8_t kline_startbyte = 0;
    uint8_t kline_tester_id = 0;
    uint8_t kline_target_id = 0;

    QByteArray ssm_receive_header_start = { "\x80\xf0\x10" };

    QStringList serial_port_list;
    QString openedSerialPort;
    QString subaru_02_16bit_bootloader_baudrate = "9600";
    QString subaru_04_16bit_bootloader_baudrate = "15625";
    QString subaru_02_32bit_bootloader_baudrate = "9600";
    QString subaru_04_32bit_bootloader_baudrate = "";
    QString subaru_05_32bit_bootloader_baudrate = "";

    QString subaru_02_16bit_kernel_baudrate = "39473";
    QString subaru_04_16bit_kernel_baudrate = "39473";
    QString subaru_02_32bit_kernel_baudrate = "62500";
    QString subaru_04_32bit_kernel_baudrate = "62500";
    QString subaru_05_32bit_kernel_baudrate = "62500";

    QString can_speed = "500000";

    uint8_t serial_port_parity = (uint8_t)QSerialPort::NoParity;
    QString serial_port_baudrate = "4800";
    QString serial_port_linux = "/dev/ttyUSB0";
    QString serial_port_windows = "COM67";
    QString serial_port;
    QString serial_port_prefix;
    QString serial_port_prefix_linux = "/dev/";
    QString serial_port_prefix_win;

    uint32_t can_source_address = 0;
    uint32_t can_destination_address = 0;
    uint32_t iso15765_source_address = 0;
    uint32_t iso15765_destination_address = 0;

#define SERIAL_P1_MIN   0x00 // J2534 says this may not be changed
#define SERIAL_P1_MAX   0x01
#define SERIAL_P2_MIN   0x02 // J2534 says this may not be changed
#define SERIAL_P2_MAX   0x03 // J2534 says this may not be changed
#define SERIAL_P3_MIN   0x04
#define SERIAL_P3_MAX   0x05 // J2534 says this may not be changed
#define SERIAL_P4_MIN   0x06
#define SERIAL_P4_MAX   0x07 // J2534 says this may not be changed

    uint8_t _P1_MAX = 10;
    bool set_kline_timings(uint32_t parameter, int value) override;

    bool is_serial_port_open() override;
    int change_port_speed(QString portSpeed) override;
    QByteArray five_baud_init(QByteArray output) override;
    int fast_init(QByteArray output) override;
    int set_lec_lines(int lec1, int lec2) override;
    int pulse_lec_1_line(int timeout) override;
    int pulse_lec_2_line(int timeout) override;

    void reset_connection() override;

    QByteArray set_error();
    QByteArray read_serial_obd_data(uint16_t timeout) override;
    QByteArray read_serial_data(uint16_t timeout) override;
    QByteArray write_serial_data(QByteArray output) override;
    QByteArray write_serial_data_echo_check(QByteArray output) override;

    int clear_rx_buffer() override;
    int clear_tx_buffer() override;

    int send_periodic_j2534_data(QByteArray output, int timeout) override;
    int stop_periodic_j2534_data() override;

    QStringList check_serial_ports() override;
    QString open_serial_port() override;

    unsigned long read_vbatt() override;
    int set_j2534_ioctl(unsigned long parameter, int value);

    bool get_is_tx_done() override;

    // -- SerialBackend ----------------------------------------------------
    QObject *qobject() override { return this; }
    // The interface takes uint32_t; the long-standing member takes unsigned
    // long. Distinct overloads on LP64 — adapt explicitly.
    int set_j2534_ioctl(uint32_t parameter, int value) override
    { return set_j2534_ioctl((unsigned long)parameter, value); }

    bool get_serialPortAvailable() override { return serialPortAvailable; }
    bool set_serialPortAvailable(bool value) override { serialPortAvailable = value; return true; }
    bool get_setRequestToSend() override { return setRequestToSend; }
    bool set_setRequestToSend(bool value) override { setRequestToSend = value; return true; }
    bool get_setDataTerminalReady() override { return setDataTerminalReady; }
    bool set_setDataTerminalReady(bool value) override { setDataTerminalReady = value; return true; }
    bool get_add_ssm_header() override { return add_ssm_header; }
    bool set_add_ssm_header(bool value) override { add_ssm_header = value; return true; }
    bool get_add_iso9141_header() override { return add_iso9141_header; }
    bool set_add_iso9141_header(bool value) override { add_iso9141_header = value; return true; }
    bool get_add_iso14230_header() override { return add_iso14230_header; }
    bool set_add_iso14230_header(bool value) override { add_iso14230_header = value; return true; }
    bool get_is_iso14230_connection() override { return is_iso14230_connection; }
    bool set_is_iso14230_connection(bool value) override { is_iso14230_connection = value; return true; }
    bool get_is_can_connection() override { return is_can_connection; }
    bool set_is_can_connection(bool value) override { is_can_connection = value; return true; }
    bool get_is_iso15765_connection() override { return is_iso15765_connection; }
    bool set_is_iso15765_connection(bool value) override { is_iso15765_connection = value; return true; }
    bool get_is_29_bit_id() override { return is_29_bit_id; }
    bool set_is_29_bit_id(bool value) override { is_29_bit_id = value; return true; }
    bool get_use_openport2_adapter() override { return use_openport2_adapter; }
    bool set_use_openport2_adapter(bool value) override { use_openport2_adapter = value; return true; }

    int  get_requestToSendEnabled() override { return requestToSendEnabled; }
    bool set_requestToSendEnabled(int value) override { requestToSendEnabled = value; return true; }
    int  get_requestToSendDisabled() override { return requestToSendDisabled; }
    bool set_requestToSendDisabled(int value) override { requestToSendDisabled = value; return true; }
    int  get_dataTerminalEnabled() override { return dataTerminalEnabled; }
    bool set_dataTerminalEnabled(int value) override { dataTerminalEnabled = value; return true; }
    int  get_dataTerminalDisabled() override { return dataTerminalDisabled; }
    bool set_dataTerminalDisabled(int value) override { dataTerminalDisabled = value; return true; }

    uint8_t get_kline_startbyte() override { return kline_startbyte; }
    bool    set_kline_startbyte(uint8_t value) override { kline_startbyte = value; return true; }
    uint8_t get_kline_tester_id() override { return kline_tester_id; }
    bool    set_kline_tester_id(uint8_t value) override { kline_tester_id = value; return true; }
    uint8_t get_kline_target_id() override { return kline_target_id; }
    bool    set_kline_target_id(uint8_t value) override { kline_target_id = value; return true; }
    uint8_t get_serial_port_parity() override { return serial_port_parity; }
    bool    set_serial_port_parity(uint8_t parity) override { serial_port_parity = parity; return true; }

    QByteArray get_ssm_receive_header_start() override { return ssm_receive_header_start; }
    bool       set_ssm_receive_header_start(QByteArray value) override { ssm_receive_header_start = value; return true; }

    QStringList get_serial_port_list() override { return serial_port_list; }
    bool        set_serial_port_list(QStringList value) override { serial_port_list = value; return true; }

    QString get_openedSerialPort() override { return openedSerialPort; }
    bool    set_openedSerialPort(QString value) override { openedSerialPort = value; return true; }
    QString get_subaru_02_16bit_bootloader_baudrate() override { return subaru_02_16bit_bootloader_baudrate; }
    bool    set_subaru_02_16bit_bootloader_baudrate(QString value) override { subaru_02_16bit_bootloader_baudrate = value; return true; }
    QString get_subaru_04_16bit_bootloader_baudrate() override { return subaru_04_16bit_bootloader_baudrate; }
    bool    set_subaru_04_16bit_bootloader_baudrate(QString value) override { subaru_04_16bit_bootloader_baudrate = value; return true; }
    QString get_subaru_02_32bit_bootloader_baudrate() override { return subaru_02_32bit_bootloader_baudrate; }
    bool    set_subaru_02_32bit_bootloader_baudrate(QString value) override { subaru_02_32bit_bootloader_baudrate = value; return true; }
    QString get_subaru_04_32bit_bootloader_baudrate() override { return subaru_04_32bit_bootloader_baudrate; }
    bool    set_subaru_04_32bit_bootloader_baudrate(QString value) override { subaru_04_32bit_bootloader_baudrate = value; return true; }
    QString get_subaru_05_32bit_bootloader_baudrate() override { return subaru_05_32bit_bootloader_baudrate; }
    bool    set_subaru_05_32bit_bootloader_baudrate(QString value) override { subaru_05_32bit_bootloader_baudrate = value; return true; }
    QString get_subaru_02_16bit_kernel_baudrate() override { return subaru_02_16bit_kernel_baudrate; }
    bool    set_subaru_02_16bit_kernel_baudrate(QString value) override { subaru_02_16bit_kernel_baudrate = value; return true; }
    QString get_subaru_04_16bit_kernel_baudrate() override { return subaru_04_16bit_kernel_baudrate; }
    bool    set_subaru_04_16bit_kernel_baudrate(QString value) override { subaru_04_16bit_kernel_baudrate = value; return true; }
    QString get_subaru_02_32bit_kernel_baudrate() override { return subaru_02_32bit_kernel_baudrate; }
    bool    set_subaru_02_32bit_kernel_baudrate(QString value) override { subaru_02_32bit_kernel_baudrate = value; return true; }
    QString get_subaru_04_32bit_kernel_baudrate() override { return subaru_04_32bit_kernel_baudrate; }
    bool    set_subaru_04_32bit_kernel_baudrate(QString value) override { subaru_04_32bit_kernel_baudrate = value; return true; }
    QString get_subaru_05_32bit_kernel_baudrate() override { return subaru_05_32bit_kernel_baudrate; }
    bool    set_subaru_05_32bit_kernel_baudrate(QString value) override { subaru_05_32bit_kernel_baudrate = value; return true; }
    QString get_can_speed() override { return can_speed; }
    bool    set_can_speed(QString value) override { can_speed = value; return true; }
    QString get_serial_port_baudrate() override { return serial_port_baudrate; }
    bool    set_serial_port_baudrate(QString value) override { serial_port_baudrate = value; return true; }
    QString get_serial_port_linux() override { return serial_port_linux; }
    bool    set_serial_port_linux(QString value) override { serial_port_linux = value; return true; }
    QString get_serial_port_windows() override { return serial_port_windows; }
    bool    set_serial_port_windows(QString value) override { serial_port_windows = value; return true; }
    QString get_serial_port() override { return serial_port; }
    bool    set_serial_port(QString value) override { serial_port = value; return true; }
    QString get_serial_port_prefix() override { return serial_port_prefix; }
    bool    set_serial_port_prefix(QString value) override { serial_port_prefix = value; return true; }
    QString get_serial_port_prefix_linux() override { return serial_port_prefix_linux; }
    bool    set_serial_port_prefix_linux(QString value) override { serial_port_prefix_linux = value; return true; }
    QString get_serial_port_prefix_win() override { return serial_port_prefix_win; }
    bool    set_serial_port_prefix_win(QString value) override { serial_port_prefix_win = value; return true; }

    uint32_t get_can_source_address() override { return can_source_address; }
    bool     set_can_source_address(uint32_t value) override { can_source_address = value; return true; }
    uint32_t get_can_destination_address() override { return can_destination_address; }
    bool     set_can_destination_address(uint32_t value) override { can_destination_address = value; return true; }
    uint32_t get_iso15765_source_address() override { return iso15765_source_address; }
    bool     set_iso15765_source_address(uint32_t value) override { iso15765_source_address = value; return true; }
    uint32_t get_iso15765_destination_address() override { return iso15765_destination_address; }
    bool     set_iso15765_destination_address(uint32_t value) override { iso15765_destination_address = value; return true; }

private:
#ifndef ARRAYSIZE
#define ARRAYSIZE(A) (sizeof(A)/sizeof((A)[0]))
#endif

    long PassThruOpen(const void *pName, unsigned long *pDeviceID);
    long PassThruClose(unsigned long DeviceID);
    long PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID, unsigned long Flags, unsigned long Baudrate, unsigned long *pChannelID);
    long PassThruDisconnect(unsigned long ChannelID);
    long PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs, unsigned long Timeout);
    long PassThruWriteMsgs(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs, unsigned long Timeout);
    long PassThruStartPeriodicMsg(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pMsgID, unsigned long TimeInterval);
    long PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID);
    long PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType, const PASSTHRU_MSG *pMaskMsg, const PASSTHRU_MSG *pPatternMsg, const PASSTHRU_MSG *pFlowControlMsg, unsigned long *pMsgID);
    long PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID);
    long PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin, unsigned long Voltage);
    long PassThruReadVersion(char *pApiVersion,char *pDllVersion,char *pFirmwareVersion,unsigned long DeviceID);
    long PassThruGetLastError(char *pErrorDescription);
    long PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput, void *pOutput);

    int set_j2534_can();
    int unset_j2534_can();
    int set_j2534_can_filters();
    //    int set_j2534_stmin_tx();
    int set_j2534_can_timings();
    int set_j2534_iso9141();
    int set_j2534_iso9141_filters();
    int set_j2534_iso9141_timings();

    unsigned long msgID = 0;

    enum rx_msg_type {
        NORM_MSG,
        TX_DONE_MSG = 0x10,
        TX_LB_MSG = 0x20,
        RX_MSG_END_IND = 0x40,
        EXT_ADDR_MSG_END_IND = 0x44,
        LB_MSG_END_IND = 0x60,
        NORM_MSG_START_IND = 0x80,
        TX_LB_START_IND = 0xA0,
    };

#define STATUS_SUCCESS							0x00
#define STATUS_ERROR							0x01

protected:
    // protected so tests can drive the J2534 lifetime (reset_connection
    // use-after-free reproduction) and the connect sequence over a mock serial.
    int init_j2534_connection();
    J2534 *j2534;

private:
    QSerialPort *serial;

    // Reentrancy guard: >0 while a J2534 read is in-flight (its read paths pump
    // the Qt event loop). close_j2534_serial_port() must not free j2534 while a
    // read holds it on the stack, or the in-flight read uses freed memory.
    int j2534_io_depth_ = 0;

    unsigned int baudrate = 4800;
    unsigned long devID = 0;
    unsigned long chanID;
    unsigned long flags;
    unsigned int parity = NO_PARITY;
    unsigned int timeout = 20;

    bool ssm_init_ok = false;

    void close_j2534_serial_port();
    bool get_serial_num(char* serial);
    void dump_msg(PASSTHRU_MSG* msg);
    void reportJ2534Error();

#if defined Q_OS_UNIX
    unsigned int protocol = ISO9141;
#elif defined Q_OS_WIN32
    unsigned int protocol = ISO9141;
#endif

    bool J2534_init_ok = false;
    bool J2534_open_ok = false;
    bool J2534_connect_ok = false;
    bool J2534_get_version_ok = false;
    bool J2534_timing_ok = false;
    bool J2534_filters_ok = false;

    bool J2534_is_denso_dsti = false;

    int line_end_check_1_toggled(int state);
    int line_end_check_2_toggled(int state);
#ifdef Q_OS_WIN32
    QMap<QString, QString> installed_drivers;
#endif

    QByteArray append_ssm_header(QByteArray output);
    QByteArray append_iso9141_header(QByteArray output);
    QByteArray append_iso14230_header(QByteArray output);
    int write_j2534_data(QByteArray output);
    QByteArray read_j2534_data(unsigned long timeout);
    QString parse_message_to_hex(QByteArray received);

    /*public slots:
    QStringList check_serial_ports();
    QString open_serial_port();*/

#ifdef WIN32
    QMap<QString, QString> getAllJ2534DriversNames();
#endif
    QStringList check_j2534_devices(QMap<QString, QString> installed_drivers);

private slots:

    void close_serial_port();
    void handle_error(QSerialPort::SerialPortError error);
    void accurate_delay(double timeout);
    void fast_delay(int timeout);
    void delay(int timeout);

};

#endif // SERIAL_PORT_ACTIONS_DIRECT_H
