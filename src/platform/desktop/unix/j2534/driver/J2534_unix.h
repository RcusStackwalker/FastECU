#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QObject>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTime>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "src/platform/desktop/unix/j2534/J2534_tactrix_unix.h"
#include "src/platform/desktop/unix/j2534/serial_byte_buffer.h"

// Note: J2534 derives from QObject (not QWidget) — it has no widget behaviour,
// only Q_OBJECT signals. QObject also lets it be constructed in a headless
// (QCoreApplication) test without a GUI platform.
class J2534 : public QObject
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
    explicit J2534();
    ~J2534();

    static constexpr std::string_view kDllVersion{"3.0.0"};
    static constexpr std::string_view kApiVersion{"04.04"};

    // PassThruReadVersion's pApiVersion/pDllVersion/pFirmwareVersion out-parameters
    // must each point at a buffer of at least this many bytes; the implementation
    // clamps to it rather than trusting source-side lengths.
    static constexpr std::size_t kVersionBufferSize = 256;

    bool serial_port_protocol_iso14230 = false;
    bool j2534_init_ok = false;

    bool IsSerialPortOpen();

    bool Init()
    {
        return true;
    };
    void SetDllName(const char * /*name*/) {}; // For Win/Linux compatibility only
    void GetDllName(char * /*name*/) {};       // For Win/Linux compatibility only
    void Debug(bool enable)
    {
        debug_mode_ = enable;
    };

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

    QString OpenSerialPort(const QString& serial_port);
    void CloseSerialPort();
    QByteArray ReadSerialData(std::uint32_t datalen, std::uint16_t timeout);
    int WriteSerialData(const QByteArray& output);
    QString ParseMessageToHex(const QByteArray& received);
    std::uint32_t ParseTs(const char *data);
    bool GetIsTxDone();

  private:
    bool debug_mode_{};

    QString opened_serial_port_;
    QString serial_port_baudrate_ = "4800";

    std::uint16_t receive_timeout_ = 500;
    std::uint16_t serial_read_timeout_ = 2000;
    std::uint16_t serial_read_extra_short_timeout_ = 50;
    std::uint16_t serial_read_short_timeout_ = 200;
    std::uint16_t serial_read_medium_timeout_ = 500;
    std::uint16_t serial_read_long_timeout_ = 800;
    std::uint16_t serial_read_extra_long_timeout_ = 3000;

  protected:
    // protected (not private) so tests can subclass J2534 and drive it into the
    // torn-down state (serial == nullptr) that the crash report exhibits.
    QSerialPort *serial_ = new QSerialPort();

  private:
    unsigned long periodic_msg_id_{};

    bool msg_ack_ = false;
    bool is_tx_done_ = false;

    SerialByteBuffer rx_buffer_;

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

    // Progress of one PassThruReadMsgs call across adapter frames.
    struct RxState
    {
        PassThruMsg *msg;
        unsigned long timeout;
        unsigned long msg_cnt = 0;
        unsigned long chunk_cnt = 0;
        unsigned long msg_byte_cnt = 0;
        std::uint32_t msg_index = 0;
    };
    enum class FrameOutcome
    {
        kContinue, // read the next frame
        kStop,     // a whole message is assembled
        kReturn,   // an ioctl reply line is in the message
    };
    FrameOutcome HandleAdapterFrame(QByteArray& received, RxState& state);
    QByteArray ReadUntil(char terminator, unsigned long timeout);
    void ReadFastInitReply(RxState& state);
    FrameOutcome HandleRxFrame(QByteArray& received, RxState& state);
    FrameOutcome HandleNormFrame(QByteArray& received, RxState& state);
    static void CopyTimestampedPayload(const QByteArray& received, RxState& state);
    static void CopyPayload(const QByteArray& received, qsizetype offset, RxState& state);
    FrameOutcome FinishMessage(QByteArray& received, RxState& state);

    void SendConfig(unsigned long channel_id, const SConfigList& config);
    long ReadVbatt(unsigned long channel_id, unsigned long *v_batt);
    long FiveBaudInit(unsigned long channel_id, const SByteArray& address, SByteArray& response);
    void SendFastInit(unsigned long channel_id, const PassThruMsg& request);

    void Delay(int n);

  private slots:
    // NOLINTBEGIN(readability-identifier-naming): Qt slots keep Qt's camelBack names
    void handleError(QSerialPort::SerialPortError error);
    // NOLINTEND(readability-identifier-naming)
};
