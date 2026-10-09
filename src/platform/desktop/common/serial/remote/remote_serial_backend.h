#pragma once

#include <QObject>
#include <QtRemoteObjects/qremoteobjectnode.h>

#include "src/platform/desktop/common/serial/serial_backend.h"
#include "src/platform/desktop/common/serial/websocket/websocketiodevice.h"

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
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State old_state);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  public:
    explicit RemoteSerialBackend(QString peer_address, QString password, QWebSocket *external_socket = nullptr,
                                 QObject *parent = nullptr);
    ~RemoteSerialBackend() override;

    QObject *Qobject() override
    {
        return this;
    }
    void WaitForSource() override;

    // -- config get/set pairs (44) --------------------------------------
    bool GetSerialPortAvailable() override;
    bool SetSerialPortAvailable(bool value) override;
    bool GetSetRequestToSend() override;
    bool SetSetRequestToSend(bool value) override;
    bool GetSetDataTerminalReady() override;
    bool SetSetDataTerminalReady(bool value) override;
    bool GetAddSsmHeader() override;
    bool SetAddSsmHeader(bool value) override;
    bool GetAddIso9141Header() override;
    bool SetAddIso9141Header(bool value) override;
    bool GetAddIso14230Header() override;
    bool SetAddIso14230Header(bool value) override;
    bool GetIsIso14230Connection() override;
    bool SetIsIso14230Connection(bool value) override;
    bool GetIsCanConnection() override;
    bool SetIsCanConnection(bool value) override;
    bool GetIsIso15765Connection() override;
    bool SetIsIso15765Connection(bool value) override;
    bool GetIs29BitId() override;
    bool SetIs29BitId(bool value) override;
    bool GetUseOpenport2Adapter() override;
    bool SetUseOpenport2Adapter(bool value) override;

    int GetRequestToSendEnabled() override;
    bool SetRequestToSendEnabled(int value) override;
    int GetRequestToSendDisabled() override;
    bool SetRequestToSendDisabled(int value) override;
    int GetDataTerminalEnabled() override;
    bool SetDataTerminalEnabled(int value) override;
    int GetDataTerminalDisabled() override;
    bool SetDataTerminalDisabled(int value) override;

    uint8_t GetKlineStartbyte() override;
    bool SetKlineStartbyte(uint8_t value) override;
    uint8_t GetKlineTesterId() override;
    bool SetKlineTesterId(uint8_t value) override;
    uint8_t GetKlineTargetId() override;
    bool SetKlineTargetId(uint8_t value) override;
    uint8_t GetSerialPortParity() override;
    bool SetSerialPortParity(uint8_t parity) override;

    QByteArray GetSsmReceiveHeaderStart() override;
    bool SetSsmReceiveHeaderStart(QByteArray value) override;

    QStringList GetSerialPortList() override;
    bool SetSerialPortList(QStringList value) override;

    QString GetOpenedSerialPort() override;
    bool SetOpenedSerialPort(QString value) override;
    QString GetSubaru0216bitBootloaderBaudrate() override;
    bool SetSubaru0216bitBootloaderBaudrate(QString value) override;
    QString GetSubaru0416bitBootloaderBaudrate() override;
    bool SetSubaru0416bitBootloaderBaudrate(QString value) override;
    QString GetSubaru0232bitBootloaderBaudrate() override;
    bool SetSubaru0232bitBootloaderBaudrate(QString value) override;
    QString GetSubaru0432bitBootloaderBaudrate() override;
    bool SetSubaru0432bitBootloaderBaudrate(QString value) override;
    QString GetSubaru0532bitBootloaderBaudrate() override;
    bool SetSubaru0532bitBootloaderBaudrate(QString value) override;
    QString GetSubaru0216bitKernelBaudrate() override;
    bool SetSubaru0216bitKernelBaudrate(QString value) override;
    QString GetSubaru0416bitKernelBaudrate() override;
    bool SetSubaru0416bitKernelBaudrate(QString value) override;
    QString GetSubaru0232bitKernelBaudrate() override;
    bool SetSubaru0232bitKernelBaudrate(QString value) override;
    QString GetSubaru0432bitKernelBaudrate() override;
    bool SetSubaru0432bitKernelBaudrate(QString value) override;
    QString GetSubaru0532bitKernelBaudrate() override;
    bool SetSubaru0532bitKernelBaudrate(QString value) override;
    QString GetCanSpeed() override;
    bool SetCanSpeed(QString value) override;
    QString GetSerialPortBaudrate() override;
    bool SetSerialPortBaudrate(QString value) override;
    QString GetSerialPortLinux() override;
    bool SetSerialPortLinux(QString value) override;
    QString GetSerialPortWindows() override;
    bool SetSerialPortWindows(QString value) override;
    QString GetSerialPort() override;
    bool SetSerialPort(QString value) override;
    QString GetSerialPortPrefix() override;
    bool SetSerialPortPrefix(QString value) override;
    QString GetSerialPortPrefixLinux() override;
    bool SetSerialPortPrefixLinux(QString value) override;
    QString GetSerialPortPrefixWin() override;
    bool SetSerialPortPrefixWin(QString value) override;

    uint32_t GetCanSourceAddress() override;
    bool SetCanSourceAddress(uint32_t value) override;
    uint32_t GetCanDestinationAddress() override;
    bool SetCanDestinationAddress(uint32_t value) override;
    uint32_t GetIso15765SourceAddress() override;
    bool SetIso15765SourceAddress(uint32_t value) override;
    uint32_t GetIso15765DestinationAddress() override;
    bool SetIso15765DestinationAddress(uint32_t value) override;

    // -- operations ------------------------------------------------------
    bool IsSerialPortOpen() override;
    int ChangePortSpeed(QString port_speed) override;
    bool SetKlineTimings(uint32_t parameter, int value) override;
    int SetJ2534Ioctl(uint32_t parameter, int value) override;
    QByteArray FiveBaudInit(QByteArray output) override;
    int FastInit(QByteArray output) override;
    int SetLecLines(int lec1, int lec2) override;
    int PulseLec1Line(int timeout) override;
    int PulseLec2Line(int timeout) override;
    void ResetConnection() override;
    QByteArray ReadSerialObdData(uint16_t timeout) override;
    QByteArray ReadSerialData(uint16_t timeout) override;
    QByteArray WriteSerialData(QByteArray output) override;
    QByteArray WriteSerialDataEchoCheck(QByteArray output) override;
    bool GetIsTxDone() override;
    int ClearRxBuffer() override;
    int ClearTxBuffer() override;
    int SendPeriodicJ2534Data(QByteArray output, int timeout) override;
    int StopPeriodicJ2534Data() override;
    QStringList CheckSerialPorts() override;
    QString OpenSerialPort() override;
    unsigned long ReadVbatt() override;

  private:
    QString peer_address_;
    QString password_;

    const QString autodiscovery_message_ = "FastECU_PTP_Autodiscovery";
    const QString remote_object_name_ = "FastECU";
    const QString wss_path_ = "/" + remote_object_name_;
    const QString web_socket_password_header_ = "fastecu-basic-password";
    const int heartbeat_interval_ = 0;

    QWebSocket *web_socket_ = nullptr;
    WebSocketIoDevice *socket_ = nullptr;
    QRemoteObjectNode node_;
    SerialPortActionsRemoteReplica *serial_remote_ = nullptr;

    void StartRemote();
    void StartOverNetwork();
    void StartLocal();
    void SendAutoDiscoveryMessage();

  private slots:
    void websocket_connected();
    void serialRemoteStateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State old_state);
};
