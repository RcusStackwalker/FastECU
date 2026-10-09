#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <cstdint>

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
    virtual QObject *Qobject() = 0;

    // -- config get/set pairs (44) --------------------------------------
    virtual bool GetSerialPortAvailable() = 0;
    virtual bool SetSerialPortAvailable(bool value) = 0;
    virtual bool GetSetRequestToSend() = 0;
    virtual bool SetSetRequestToSend(bool value) = 0;
    virtual bool GetSetDataTerminalReady() = 0;
    virtual bool SetSetDataTerminalReady(bool value) = 0;
    virtual bool GetAddSsmHeader() = 0;
    virtual bool SetAddSsmHeader(bool value) = 0;
    virtual bool GetAddIso9141Header() = 0;
    virtual bool SetAddIso9141Header(bool value) = 0;
    virtual bool GetAddIso14230Header() = 0;
    virtual bool SetAddIso14230Header(bool value) = 0;
    virtual bool GetIsIso14230Connection() = 0;
    virtual bool SetIsIso14230Connection(bool value) = 0;
    virtual bool GetIsCanConnection() = 0;
    virtual bool SetIsCanConnection(bool value) = 0;
    virtual bool GetIsIso15765Connection() = 0;
    virtual bool SetIsIso15765Connection(bool value) = 0;
    virtual bool GetIs29BitId() = 0;
    virtual bool SetIs29BitId(bool value) = 0;
    virtual bool GetUseOpenport2Adapter() = 0;
    virtual bool SetUseOpenport2Adapter(bool value) = 0;

    virtual int GetRequestToSendEnabled() = 0;
    virtual bool SetRequestToSendEnabled(int value) = 0;
    virtual int GetRequestToSendDisabled() = 0;
    virtual bool SetRequestToSendDisabled(int value) = 0;
    virtual int GetDataTerminalEnabled() = 0;
    virtual bool SetDataTerminalEnabled(int value) = 0;
    virtual int GetDataTerminalDisabled() = 0;
    virtual bool SetDataTerminalDisabled(int value) = 0;

    virtual std::uint8_t GetKlineStartbyte() = 0;
    virtual bool SetKlineStartbyte(std::uint8_t value) = 0;
    virtual std::uint8_t GetKlineTesterId() = 0;
    virtual bool SetKlineTesterId(std::uint8_t value) = 0;
    virtual std::uint8_t GetKlineTargetId() = 0;
    virtual bool SetKlineTargetId(std::uint8_t value) = 0;
    virtual std::uint8_t GetSerialPortParity() = 0;
    virtual bool SetSerialPortParity(std::uint8_t parity) = 0;

    virtual QByteArray GetSsmReceiveHeaderStart() = 0;
    virtual bool SetSsmReceiveHeaderStart(QByteArray value) = 0;

    virtual QStringList GetSerialPortList() = 0;
    virtual bool SetSerialPortList(QStringList value) = 0;

    virtual QString GetOpenedSerialPort() = 0;
    virtual bool SetOpenedSerialPort(QString value) = 0;
    virtual QString GetSubaru0216bitBootloaderBaudrate() = 0;
    virtual bool SetSubaru0216bitBootloaderBaudrate(QString value) = 0;
    virtual QString GetSubaru0416bitBootloaderBaudrate() = 0;
    virtual bool SetSubaru0416bitBootloaderBaudrate(QString value) = 0;
    virtual QString GetSubaru0232bitBootloaderBaudrate() = 0;
    virtual bool SetSubaru0232bitBootloaderBaudrate(QString value) = 0;
    virtual QString GetSubaru0432bitBootloaderBaudrate() = 0;
    virtual bool SetSubaru0432bitBootloaderBaudrate(QString value) = 0;
    virtual QString GetSubaru0532bitBootloaderBaudrate() = 0;
    virtual bool SetSubaru0532bitBootloaderBaudrate(QString value) = 0;
    virtual QString GetSubaru0216bitKernelBaudrate() = 0;
    virtual bool SetSubaru0216bitKernelBaudrate(QString value) = 0;
    virtual QString GetSubaru0416bitKernelBaudrate() = 0;
    virtual bool SetSubaru0416bitKernelBaudrate(QString value) = 0;
    virtual QString GetSubaru0232bitKernelBaudrate() = 0;
    virtual bool SetSubaru0232bitKernelBaudrate(QString value) = 0;
    virtual QString GetSubaru0432bitKernelBaudrate() = 0;
    virtual bool SetSubaru0432bitKernelBaudrate(QString value) = 0;
    virtual QString GetSubaru0532bitKernelBaudrate() = 0;
    virtual bool SetSubaru0532bitKernelBaudrate(QString value) = 0;
    virtual QString GetCanSpeed() = 0;
    virtual bool SetCanSpeed(QString value) = 0;
    virtual QString GetSerialPortBaudrate() = 0;
    virtual bool SetSerialPortBaudrate(QString value) = 0;
    virtual QString GetSerialPortLinux() = 0;
    virtual bool SetSerialPortLinux(QString value) = 0;
    virtual QString GetSerialPortWindows() = 0;
    virtual bool SetSerialPortWindows(QString value) = 0;
    virtual QString GetSerialPort() = 0;
    virtual bool SetSerialPort(QString value) = 0;
    virtual QString GetSerialPortPrefix() = 0;
    virtual bool SetSerialPortPrefix(QString value) = 0;
    virtual QString GetSerialPortPrefixLinux() = 0;
    virtual bool SetSerialPortPrefixLinux(QString value) = 0;
    virtual QString GetSerialPortPrefixWin() = 0;
    virtual bool SetSerialPortPrefixWin(QString value) = 0;

    virtual std::uint32_t GetCanSourceAddress() = 0;
    virtual bool SetCanSourceAddress(std::uint32_t value) = 0;
    virtual std::uint32_t GetCanDestinationAddress() = 0;
    virtual bool SetCanDestinationAddress(std::uint32_t value) = 0;
    virtual std::uint32_t GetIso15765SourceAddress() = 0;
    virtual bool SetIso15765SourceAddress(std::uint32_t value) = 0;
    virtual std::uint32_t GetIso15765DestinationAddress() = 0;
    virtual bool SetIso15765DestinationAddress(std::uint32_t value) = 0;

    // -- operations ------------------------------------------------------
    virtual bool IsSerialPortOpen() = 0;
    virtual int ChangePortSpeed(QString port_speed) = 0;
    virtual bool SetKlineTimings(std::uint32_t parameter, int value) = 0;
    virtual int SetJ2534Ioctl(std::uint32_t parameter, int value) = 0;
    virtual QByteArray FiveBaudInit(QByteArray output) = 0;
    virtual int FastInit(QByteArray output) = 0;
    virtual int SetLecLines(int lec1, int lec2) = 0;
    virtual int PulseLec1Line(int timeout) = 0;
    virtual int PulseLec2Line(int timeout) = 0;
    virtual void ResetConnection() = 0;
    virtual QByteArray ReadSerialObdData(std::uint16_t timeout) = 0;
    virtual QByteArray ReadSerialData(std::uint16_t timeout) = 0;
    virtual QByteArray WriteSerialData(QByteArray output) = 0;
    virtual QByteArray WriteSerialDataEchoCheck(QByteArray output) = 0;
    virtual bool GetIsTxDone() = 0;
    virtual int ClearRxBuffer() = 0;
    virtual int ClearTxBuffer() = 0;
    virtual int SendPeriodicJ2534Data(QByteArray output, int timeout) = 0;
    virtual int StopPeriodicJ2534Data() = 0;
    virtual QStringList CheckSerialPorts() = 0;
    virtual QString OpenSerialPort() = 0;
    virtual unsigned long ReadVbatt() = 0;

    // Remote-only: block until the QtRO source is replicated. No-op for the
    // direct backend.
    virtual void WaitForSource()
    {
    }
};
