#pragma once

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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <thread>
#include <chrono>

#include "src/platform/desktop/j2534/j2534_api.h"

#include "src/platform/desktop/common/serial/serial_backend.h"
#include "src/platform/desktop/common/serial/serial_facade_codes.h"

class SerialPortActionsDirect : public QObject, public SerialBackend
{
    Q_OBJECT

  signals:
    // NOLINTBEGIN(readability-identifier-naming): Qt signals keep Qt's camelBack names
    void logE(QString message, bool timestamp, bool linefeed);
    void logW(QString message, bool timestamp, bool linefeed);
    void logI(QString message, bool timestamp, bool linefeed);
    void logD(QString message, bool timestamp, bool linefeed);
    // NOLINTEND(readability-identifier-naming)

  public:
    explicit SerialPortActionsDirect(QObject *parent = nullptr);
    ~SerialPortActionsDirect();

    bool serial_port_available = false;
    bool set_request_to_send = true;
    bool set_data_terminal_ready = true;
    bool signal_to_read_batt_voltage = false;

    bool add_ssm_header = false;
    bool add_iso9141_header = false;
    bool add_iso14230_header = false;
    bool is_iso14230_connection = false;
    bool is_can_connection = false;
    bool is_iso15765_connection = false;
    bool is_29_bit_id = false;

    bool use_openport2_adapter = false;

    unsigned long v_batt = 0;

    int request_to_send_enabled = 0;
    int request_to_send_disabled = 1;
    int data_terminal_enabled = 0;
    int data_terminal_disabled = 1;

    std::uint16_t echo_check_timout = 5000;
    std::uint16_t receive_timeout = 500;
    std::uint16_t serial_read_timeout = 2000;
    std::uint16_t serial_read_extra_short_timeout = 50;
    std::uint16_t serial_read_short_timeout = 200;
    std::uint16_t serial_read_medium_timeout = 500;
    std::uint16_t serial_read_long_timeout = 800;
    std::uint16_t serial_read_extra_long_timeout = 3000;

    std::uint8_t kline_startbyte = 0;
    std::uint8_t kline_tester_id = 0;
    std::uint8_t kline_target_id = 0;

    QByteArray ssm_receive_header_start = {"\x80\xf0\x10"};

    QStringList serial_port_list;
    QString opened_serial_port;
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

    std::uint8_t serial_port_parity = (std::uint8_t)QSerialPort::NoParity;
    QString serial_port_baudrate = "4800";
    QString serial_port_linux = "/dev/ttyUSB0";
    QString serial_port_windows = "COM67";
    QString serial_port;
    QString serial_port_prefix;
    QString serial_port_prefix_linux = "/dev/";
    QString serial_port_prefix_win;

    std::uint32_t can_source_address = 0;
    std::uint32_t can_destination_address = 0;
    std::uint32_t iso15765_source_address = 0;
    std::uint32_t iso15765_destination_address = 0;

    std::uint8_t p1_max_ms = 10;
    bool SetKlineTimings(std::uint32_t parameter, int value) override;

    bool IsSerialPortOpen() override;
    int ChangePortSpeed(QString port_speed) override;
    QByteArray FiveBaudInit(QByteArray output) override;
    int FastInit(QByteArray output) override;
    int SetLecLines(int lec1, int lec2) override;
    int PulseLec1Line(int timeout_arg) override;
    int PulseLec2Line(int timeout_arg) override;

    void ResetConnection() override;

    QByteArray SetError();
    QByteArray ReadSerialObdData(std::uint16_t timeout_arg) override;
    QByteArray ReadSerialData(std::uint16_t timeout_arg) override;
    QByteArray WriteSerialData(QByteArray output) override;
    QByteArray WriteSerialDataEchoCheck(QByteArray output) override;

    int ClearRxBuffer() override;
    int ClearTxBuffer() override;

    int SendPeriodicJ2534Data(QByteArray output, int timeout_arg) override;
    int StopPeriodicJ2534Data() override;

    QStringList CheckSerialPorts() override;
    QString OpenSerialPort() override;

    unsigned long ReadVbatt() override;
    int SetJ2534Ioctl(unsigned long parameter, int value);

    bool GetIsTxDone() override;

    // -- SerialBackend ----------------------------------------------------
    QObject *Qobject() override
    {
        return this;
    }
    // The interface takes std::uint32_t; the long-standing member takes unsigned
    // long. Distinct overloads on LP64 — adapt explicitly.
    int SetJ2534Ioctl(std::uint32_t parameter, int value) override
    {
        return SetJ2534Ioctl((unsigned long)parameter, value);
    }

    bool GetSerialPortAvailable() override
    {
        return serial_port_available;
    }
    bool SetSerialPortAvailable(bool value) override
    {
        serial_port_available = value;
        return true;
    }
    bool GetSetRequestToSend() override
    {
        return set_request_to_send;
    }
    bool SetSetRequestToSend(bool value) override
    {
        set_request_to_send = value;
        return true;
    }
    bool GetSetDataTerminalReady() override
    {
        return set_data_terminal_ready;
    }
    bool SetSetDataTerminalReady(bool value) override
    {
        set_data_terminal_ready = value;
        return true;
    }
    bool GetAddSsmHeader() override
    {
        return add_ssm_header;
    }
    bool SetAddSsmHeader(bool value) override
    {
        add_ssm_header = value;
        return true;
    }
    bool GetAddIso9141Header() override
    {
        return add_iso9141_header;
    }
    bool SetAddIso9141Header(bool value) override
    {
        add_iso9141_header = value;
        return true;
    }
    bool GetAddIso14230Header() override
    {
        return add_iso14230_header;
    }
    bool SetAddIso14230Header(bool value) override
    {
        add_iso14230_header = value;
        return true;
    }
    bool GetIsIso14230Connection() override
    {
        return is_iso14230_connection;
    }
    bool SetIsIso14230Connection(bool value) override
    {
        is_iso14230_connection = value;
        return true;
    }
    bool GetIsCanConnection() override
    {
        return is_can_connection;
    }
    bool SetIsCanConnection(bool value) override
    {
        is_can_connection = value;
        return true;
    }
    bool GetIsIso15765Connection() override
    {
        return is_iso15765_connection;
    }
    bool SetIsIso15765Connection(bool value) override
    {
        is_iso15765_connection = value;
        return true;
    }
    bool GetIs29BitId() override
    {
        return is_29_bit_id;
    }
    bool SetIs29BitId(bool value) override
    {
        is_29_bit_id = value;
        return true;
    }
    bool GetUseOpenport2Adapter() override
    {
        return use_openport2_adapter;
    }
    bool SetUseOpenport2Adapter(bool value) override
    {
        use_openport2_adapter = value;
        return true;
    }

    int GetRequestToSendEnabled() override
    {
        return request_to_send_enabled;
    }
    bool SetRequestToSendEnabled(int value) override
    {
        request_to_send_enabled = value;
        return true;
    }
    int GetRequestToSendDisabled() override
    {
        return request_to_send_disabled;
    }
    bool SetRequestToSendDisabled(int value) override
    {
        request_to_send_disabled = value;
        return true;
    }
    int GetDataTerminalEnabled() override
    {
        return data_terminal_enabled;
    }
    bool SetDataTerminalEnabled(int value) override
    {
        data_terminal_enabled = value;
        return true;
    }
    int GetDataTerminalDisabled() override
    {
        return data_terminal_disabled;
    }
    bool SetDataTerminalDisabled(int value) override
    {
        data_terminal_disabled = value;
        return true;
    }

    std::uint8_t GetKlineStartbyte() override
    {
        return kline_startbyte;
    }
    bool SetKlineStartbyte(std::uint8_t value) override
    {
        kline_startbyte = value;
        return true;
    }
    std::uint8_t GetKlineTesterId() override
    {
        return kline_tester_id;
    }
    bool SetKlineTesterId(std::uint8_t value) override
    {
        kline_tester_id = value;
        return true;
    }
    std::uint8_t GetKlineTargetId() override
    {
        return kline_target_id;
    }
    bool SetKlineTargetId(std::uint8_t value) override
    {
        kline_target_id = value;
        return true;
    }
    std::uint8_t GetSerialPortParity() override
    {
        return serial_port_parity;
    }
    bool SetSerialPortParity(std::uint8_t parity_arg) override
    {
        serial_port_parity = parity_arg;
        if (serial_ && serial_->isOpen())
        {
            return serial_->setParity(static_cast<QSerialPort::Parity>(parity_arg));
        }
        return true;
    }

    QByteArray GetSsmReceiveHeaderStart() override
    {
        return ssm_receive_header_start;
    }
    bool SetSsmReceiveHeaderStart(QByteArray value) override
    {
        ssm_receive_header_start = value;
        return true;
    }

    QStringList GetSerialPortList() override
    {
        return serial_port_list;
    }
    bool SetSerialPortList(QStringList value) override
    {
        serial_port_list = value;
        return true;
    }

    QString GetOpenedSerialPort() override
    {
        return opened_serial_port;
    }
    bool SetOpenedSerialPort(QString value) override
    {
        opened_serial_port = value;
        return true;
    }
    QString GetSubaru0216bitBootloaderBaudrate() override
    {
        return subaru_02_16bit_bootloader_baudrate;
    }
    bool SetSubaru0216bitBootloaderBaudrate(QString value) override
    {
        subaru_02_16bit_bootloader_baudrate = value;
        return true;
    }
    QString GetSubaru0416bitBootloaderBaudrate() override
    {
        return subaru_04_16bit_bootloader_baudrate;
    }
    bool SetSubaru0416bitBootloaderBaudrate(QString value) override
    {
        subaru_04_16bit_bootloader_baudrate = value;
        return true;
    }
    QString GetSubaru0232bitBootloaderBaudrate() override
    {
        return subaru_02_32bit_bootloader_baudrate;
    }
    bool SetSubaru0232bitBootloaderBaudrate(QString value) override
    {
        subaru_02_32bit_bootloader_baudrate = value;
        return true;
    }
    QString GetSubaru0432bitBootloaderBaudrate() override
    {
        return subaru_04_32bit_bootloader_baudrate;
    }
    bool SetSubaru0432bitBootloaderBaudrate(QString value) override
    {
        subaru_04_32bit_bootloader_baudrate = value;
        return true;
    }
    QString GetSubaru0532bitBootloaderBaudrate() override
    {
        return subaru_05_32bit_bootloader_baudrate;
    }
    bool SetSubaru0532bitBootloaderBaudrate(QString value) override
    {
        subaru_05_32bit_bootloader_baudrate = value;
        return true;
    }
    QString GetSubaru0216bitKernelBaudrate() override
    {
        return subaru_02_16bit_kernel_baudrate;
    }
    bool SetSubaru0216bitKernelBaudrate(QString value) override
    {
        subaru_02_16bit_kernel_baudrate = value;
        return true;
    }
    QString GetSubaru0416bitKernelBaudrate() override
    {
        return subaru_04_16bit_kernel_baudrate;
    }
    bool SetSubaru0416bitKernelBaudrate(QString value) override
    {
        subaru_04_16bit_kernel_baudrate = value;
        return true;
    }
    QString GetSubaru0232bitKernelBaudrate() override
    {
        return subaru_02_32bit_kernel_baudrate;
    }
    bool SetSubaru0232bitKernelBaudrate(QString value) override
    {
        subaru_02_32bit_kernel_baudrate = value;
        return true;
    }
    QString GetSubaru0432bitKernelBaudrate() override
    {
        return subaru_04_32bit_kernel_baudrate;
    }
    bool SetSubaru0432bitKernelBaudrate(QString value) override
    {
        subaru_04_32bit_kernel_baudrate = value;
        return true;
    }
    QString GetSubaru0532bitKernelBaudrate() override
    {
        return subaru_05_32bit_kernel_baudrate;
    }
    bool SetSubaru0532bitKernelBaudrate(QString value) override
    {
        subaru_05_32bit_kernel_baudrate = value;
        return true;
    }
    QString GetCanSpeed() override
    {
        return can_speed;
    }
    bool SetCanSpeed(QString value) override
    {
        can_speed = value;
        return true;
    }
    QString GetSerialPortBaudrate() override
    {
        return serial_port_baudrate;
    }
    bool SetSerialPortBaudrate(QString value) override
    {
        serial_port_baudrate = value;
        return true;
    }
    QString GetSerialPortLinux() override
    {
        return serial_port_linux;
    }
    bool SetSerialPortLinux(QString value) override
    {
        serial_port_linux = value;
        return true;
    }
    QString GetSerialPortWindows() override
    {
        return serial_port_windows;
    }
    bool SetSerialPortWindows(QString value) override
    {
        serial_port_windows = value;
        return true;
    }
    QString GetSerialPort() override
    {
        return serial_port;
    }
    bool SetSerialPort(QString value) override
    {
        serial_port = value;
        return true;
    }
    QString GetSerialPortPrefix() override
    {
        return serial_port_prefix;
    }
    bool SetSerialPortPrefix(QString value) override
    {
        serial_port_prefix = value;
        return true;
    }
    QString GetSerialPortPrefixLinux() override
    {
        return serial_port_prefix_linux;
    }
    bool SetSerialPortPrefixLinux(QString value) override
    {
        serial_port_prefix_linux = value;
        return true;
    }
    QString GetSerialPortPrefixWin() override
    {
        return serial_port_prefix_win;
    }
    bool SetSerialPortPrefixWin(QString value) override
    {
        serial_port_prefix_win = value;
        return true;
    }

    std::uint32_t GetCanSourceAddress() override
    {
        return can_source_address;
    }
    bool SetCanSourceAddress(std::uint32_t value) override
    {
        can_source_address = value;
        return true;
    }
    std::uint32_t GetCanDestinationAddress() override
    {
        return can_destination_address;
    }
    bool SetCanDestinationAddress(std::uint32_t value) override
    {
        can_destination_address = value;
        return true;
    }
    std::uint32_t GetIso15765SourceAddress() override
    {
        return iso15765_source_address;
    }
    bool SetIso15765SourceAddress(std::uint32_t value) override
    {
        iso15765_source_address = value;
        return true;
    }
    std::uint32_t GetIso15765DestinationAddress() override
    {
        return iso15765_destination_address;
    }
    bool SetIso15765DestinationAddress(std::uint32_t value) override
    {
        iso15765_destination_address = value;
        return true;
    }

  private:
    long PassThruOpen(const void *p_name, unsigned long *p_device_id);
    long PassThruClose(unsigned long device_id);
    long PassThruConnect(unsigned long device_id, unsigned long protocol_id, unsigned long flags,
                         unsigned long baudrate, unsigned long *p_channel_id);
    long PassThruDisconnect(unsigned long channel_id);
    long PassThruReadMsgs(unsigned long channel_id, PassThruMsg *p_msg, unsigned long *p_num_msgs,
                          unsigned long timeout);
    long PassThruWriteMsgs(unsigned long channel_id, const PassThruMsg *p_msg, unsigned long *p_num_msgs,
                           unsigned long timeout);
    long PassThruStartPeriodicMsg(unsigned long channel_id, const PassThruMsg *p_msg, unsigned long *p_msg_id,
                                  unsigned long time_interval);
    long PassThruStopPeriodicMsg(unsigned long channel_id, unsigned long msg_id);
    long PassThruStartMsgFilter(unsigned long channel_id, unsigned long filter_type, const PassThruMsg *p_mask_msg,
                                const PassThruMsg *p_pattern_msg, const PassThruMsg *p_flow_control_msg,
                                unsigned long *p_msg_id);
    long PassThruStopMsgFilter(unsigned long channel_id, unsigned long msg_id);
    long PassThruSetProgrammingVoltage(unsigned long device_id, unsigned long pin, unsigned long voltage);
    long PassThruReadVersion(char *p_api_version, char *p_dll_version, char *p_firmware_version,
                             unsigned long device_id);
    long PassThruGetLastError(char *p_error_description);
    long PassThruIoctl(unsigned long channel_id, unsigned long ioctl_id, const void *p_input, void *p_output);

    int SetJ2534Can();
    int UnsetJ2534Can();
    int SetJ2534CanFilters();
    //    int set_j2534_stmin_tx();
    int SetJ2534CanTimings();
    int SetJ2534Iso9141();
    int SetJ2534Iso9141Filters();
    int SetJ2534Iso9141Timings();

    unsigned long msg_id_ = 0;

    enum RxMsgType
    {
        kNormMsg,
        kTxDoneMsg = 0x10,
        kTxLbMsg = 0x20,
        kRxMsgEndInd = 0x40,
        kExtAddrMsgEndInd = 0x44,
        kLbMsgEndInd = 0x60,
        kNormMsgStartInd = 0x80,
        kTxLbStartInd = 0xA0,
    };

  protected:
    // protected so tests can drive the J2534 lifetime (reset_connection
    // use-after-free reproduction) and the connect sequence over a mock serial.
    int InitJ2534Connection();
    J2534 *j2534_;

    // Per-OS hooks. Declared once here and defined in exactly one of
    // serial_port_actions_direct_unix.cpp / serial_port_actions_direct_windows.cpp,
    // which the BUILD file selects; each replaces one former OS preprocessor branch.
    // Protected so tests can pin the pure ones.
    struct ResolvedPort
    {
        QString port;
        bool is_j2534 = false;
    };
    void ConnectJ2534Logs();
    void SettleAfterProgrammingVoltage();
    void AppendJ2534Interfaces(QStringList& serial_ports);
    ResolvedPort ResolvePort(const QString& entry) const;
    void SelectJ2534Dll();
    bool OpenJ2534Transport();
    void CloseJ2534Transport();
    void LogJ2534Opened();
    void AdoptJ2534ChannelId();
    bool J2534TxDone();

  private:
    QSerialPort *serial_;

    // Reentrancy guard: >0 while a J2534 read is in-flight (its read paths pump
    // the Qt event loop). close_j2534_serial_port() must not free j2534 while a
    // read holds it on the stack, or the in-flight read uses freed memory.
    int j2534_io_depth_ = 0;

    unsigned int baudrate_ = 4800;
    unsigned long dev_id_ = 0;
    unsigned long chan_id_{};
    unsigned long flags_{};
    unsigned int parity_ = kJ2534NoParity;
    unsigned int timeout_ = 20;

    bool ssm_init_ok_ = false;

    void CloseJ2534SerialPort();
    bool GetSerialNum(char *serial_arg);
    void DumpMsg(PassThruMsg *msg);
    void ReportJ2534Error();

    unsigned int protocol_ = kJ2534Iso9141;

    bool j2534_init_ok_ = false;
    bool j2534_open_ok_ = false;
    bool j2534_connect_ok_ = false;
    bool j2534_get_version_ok_ = false;
    bool j2534_timing_ok_ = false;
    bool j2534_filters_ok_ = false;

    bool j2534_is_denso_dsti_ = false;

    int LineEndCheck1Toggled(int state);
    int LineEndCheck2Toggled(int state);
    QMap<QString, QString> installed_drivers_;

    QByteArray AppendSsmHeader(QByteArray output);
    QByteArray AppendIso9141Header(QByteArray output);
    QByteArray AppendIso14230Header(QByteArray output);
    int WriteJ2534Data(QByteArray output);
    QByteArray ReadJ2534Data(unsigned long timeout_arg);
    QString ParseMessageToHex(const QByteArray& received);

    /*public slots:
    QStringList check_serial_ports();
    QString open_serial_port();*/

    QMap<QString, QString> GetAllJ2534DriversNames();
    QStringList CheckJ2534Devices(QMap<QString, QString> installed_drivers);

  private slots:

    // NOLINTBEGIN(readability-identifier-naming): Qt slots keep Qt's camelBack names
    void closeSerialPort();
    void handleError(QSerialPort::SerialPortError error);
    void accurateDelay(double timeout_arg);
    void fastDelay(int timeout_arg);
    void delay(int timeout_arg);
    // NOLINTEND(readability-identifier-naming)
};
