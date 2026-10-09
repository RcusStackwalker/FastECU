#pragma once

#include <gmock/gmock.h>

#include <cstdint>

#include "src/platform/desktop/common/serial/direct/serial_port_actions_direct.h"

// Thrown by tests that exercise an adapter's catch-all branch. It deliberately
// does not derive from std::exception, so it is distinct from a runtime_error.
struct FakeBackendNonStandardFailure
{
};

// Keep the intentional catch-all probe at a suppressible throw site rather
// than instantiating testing::Throw's throw expression in a third-party header.
ACTION(ThrowNonStandardBackendFailure)
{
    throw FakeBackendNonStandardFailure{}; // NOLINT(bugprone-std-exception-baseclass): tests catch (...).
}

// Google Mock backend for facade and desktop transport tests. What it
// guarantees, how to set expectations against it, and the Qt application
// integration it requires are in docs/gmock-reference.md.
class FakeBackend : public SerialPortActionsDirect
{

  public:
    FakeBackend()
    {
        InstallDefaultActions();
    }

    ~FakeBackend() override
    {
        if (destroyed)
        {
            *destroyed = true;
        }
    }

    bool *destroyed = nullptr;

    MOCK_METHOD(bool, GetSerialPortAvailable, (), (override));
    MOCK_METHOD(bool, SetSerialPortAvailable, (bool value), (override));
    MOCK_METHOD(bool, GetSetRequestToSend, (), (override));
    MOCK_METHOD(bool, SetSetRequestToSend, (bool value), (override));
    MOCK_METHOD(bool, GetSetDataTerminalReady, (), (override));
    MOCK_METHOD(bool, SetSetDataTerminalReady, (bool value), (override));
    MOCK_METHOD(bool, GetAddSsmHeader, (), (override));
    MOCK_METHOD(bool, SetAddSsmHeader, (bool value), (override));
    MOCK_METHOD(bool, GetAddIso9141Header, (), (override));
    MOCK_METHOD(bool, SetAddIso9141Header, (bool value), (override));
    MOCK_METHOD(bool, GetAddIso14230Header, (), (override));
    MOCK_METHOD(bool, SetAddIso14230Header, (bool value), (override));
    MOCK_METHOD(bool, GetIsIso14230Connection, (), (override));
    MOCK_METHOD(bool, SetIsIso14230Connection, (bool value), (override));
    MOCK_METHOD(bool, GetIsCanConnection, (), (override));
    MOCK_METHOD(bool, SetIsCanConnection, (bool value), (override));
    MOCK_METHOD(bool, GetIsIso15765Connection, (), (override));
    MOCK_METHOD(bool, SetIsIso15765Connection, (bool value), (override));
    MOCK_METHOD(bool, GetIs29BitId, (), (override));
    MOCK_METHOD(bool, SetIs29BitId, (bool value), (override));
    MOCK_METHOD(bool, GetUseOpenport2Adapter, (), (override));
    MOCK_METHOD(bool, SetUseOpenport2Adapter, (bool value), (override));

    MOCK_METHOD(int, GetRequestToSendEnabled, (), (override));
    MOCK_METHOD(bool, SetRequestToSendEnabled, (int value), (override));
    MOCK_METHOD(int, GetRequestToSendDisabled, (), (override));
    MOCK_METHOD(bool, SetRequestToSendDisabled, (int value), (override));
    MOCK_METHOD(int, GetDataTerminalEnabled, (), (override));
    MOCK_METHOD(bool, SetDataTerminalEnabled, (int value), (override));
    MOCK_METHOD(int, GetDataTerminalDisabled, (), (override));
    MOCK_METHOD(bool, SetDataTerminalDisabled, (int value), (override));

    MOCK_METHOD(std::uint8_t, GetKlineStartbyte, (), (override));
    MOCK_METHOD(bool, SetKlineStartbyte, (std::uint8_t value), (override));
    MOCK_METHOD(std::uint8_t, GetKlineTesterId, (), (override));
    MOCK_METHOD(bool, SetKlineTesterId, (std::uint8_t value), (override));
    MOCK_METHOD(std::uint8_t, GetKlineTargetId, (), (override));
    MOCK_METHOD(bool, SetKlineTargetId, (std::uint8_t value), (override));
    MOCK_METHOD(std::uint8_t, GetSerialPortParity, (), (override));
    MOCK_METHOD(bool, SetSerialPortParity, (std::uint8_t parity), (override));

    MOCK_METHOD(QByteArray, GetSsmReceiveHeaderStart, (), (override));
    MOCK_METHOD(bool, SetSsmReceiveHeaderStart, (QByteArray value), (override));

    MOCK_METHOD(QStringList, GetSerialPortList, (), (override));
    MOCK_METHOD(bool, SetSerialPortList, (QStringList value), (override));

    MOCK_METHOD(QString, GetOpenedSerialPort, (), (override));
    MOCK_METHOD(bool, SetOpenedSerialPort, (QString value), (override));
    MOCK_METHOD(QString, GetSubaru0216bitBootloaderBaudrate, (), (override));
    MOCK_METHOD(bool, SetSubaru0216bitBootloaderBaudrate, (QString value), (override));
    MOCK_METHOD(QString, GetSubaru0416bitBootloaderBaudrate, (), (override));
    MOCK_METHOD(bool, SetSubaru0416bitBootloaderBaudrate, (QString value), (override));
    MOCK_METHOD(QString, GetSubaru0232bitBootloaderBaudrate, (), (override));
    MOCK_METHOD(bool, SetSubaru0232bitBootloaderBaudrate, (QString value), (override));
    MOCK_METHOD(QString, GetSubaru0432bitBootloaderBaudrate, (), (override));
    MOCK_METHOD(bool, SetSubaru0432bitBootloaderBaudrate, (QString value), (override));
    MOCK_METHOD(QString, GetSubaru0532bitBootloaderBaudrate, (), (override));
    MOCK_METHOD(bool, SetSubaru0532bitBootloaderBaudrate, (QString value), (override));
    MOCK_METHOD(QString, GetSubaru0216bitKernelBaudrate, (), (override));
    MOCK_METHOD(bool, SetSubaru0216bitKernelBaudrate, (QString value), (override));
    MOCK_METHOD(QString, GetSubaru0416bitKernelBaudrate, (), (override));
    MOCK_METHOD(bool, SetSubaru0416bitKernelBaudrate, (QString value), (override));
    MOCK_METHOD(QString, GetSubaru0232bitKernelBaudrate, (), (override));
    MOCK_METHOD(bool, SetSubaru0232bitKernelBaudrate, (QString value), (override));
    MOCK_METHOD(QString, GetSubaru0432bitKernelBaudrate, (), (override));
    MOCK_METHOD(bool, SetSubaru0432bitKernelBaudrate, (QString value), (override));
    MOCK_METHOD(QString, GetSubaru0532bitKernelBaudrate, (), (override));
    MOCK_METHOD(bool, SetSubaru0532bitKernelBaudrate, (QString value), (override));
    MOCK_METHOD(QString, GetCanSpeed, (), (override));
    MOCK_METHOD(bool, SetCanSpeed, (QString value), (override));
    MOCK_METHOD(QString, GetSerialPortBaudrate, (), (override));
    MOCK_METHOD(bool, SetSerialPortBaudrate, (QString value), (override));
    MOCK_METHOD(QString, GetSerialPortLinux, (), (override));
    MOCK_METHOD(bool, SetSerialPortLinux, (QString value), (override));
    MOCK_METHOD(QString, GetSerialPortWindows, (), (override));
    MOCK_METHOD(bool, SetSerialPortWindows, (QString value), (override));
    MOCK_METHOD(QString, GetSerialPort, (), (override));
    MOCK_METHOD(bool, SetSerialPort, (QString value), (override));
    MOCK_METHOD(QString, GetSerialPortPrefix, (), (override));
    MOCK_METHOD(bool, SetSerialPortPrefix, (QString value), (override));
    MOCK_METHOD(QString, GetSerialPortPrefixLinux, (), (override));
    MOCK_METHOD(bool, SetSerialPortPrefixLinux, (QString value), (override));
    MOCK_METHOD(QString, GetSerialPortPrefixWin, (), (override));
    MOCK_METHOD(bool, SetSerialPortPrefixWin, (QString value), (override));

    MOCK_METHOD(std::uint32_t, GetCanSourceAddress, (), (override));
    MOCK_METHOD(bool, SetCanSourceAddress, (std::uint32_t value), (override));
    MOCK_METHOD(std::uint32_t, GetCanDestinationAddress, (), (override));
    MOCK_METHOD(bool, SetCanDestinationAddress, (std::uint32_t value), (override));
    MOCK_METHOD(std::uint32_t, GetIso15765SourceAddress, (), (override));
    MOCK_METHOD(bool, SetIso15765SourceAddress, (std::uint32_t value), (override));
    MOCK_METHOD(std::uint32_t, GetIso15765DestinationAddress, (), (override));
    MOCK_METHOD(bool, SetIso15765DestinationAddress, (std::uint32_t value), (override));

    MOCK_METHOD(bool, IsSerialPortOpen, (), (override));
    MOCK_METHOD(int, ChangePortSpeed, (QString port_speed), (override));
    MOCK_METHOD(bool, SetKlineTimings, (std::uint32_t parameter, int value), (override));
    MOCK_METHOD(int, SetJ2534Ioctl, (std::uint32_t parameter, int value), (override));
    MOCK_METHOD(QByteArray, FiveBaudInit, (QByteArray output), (override));
    MOCK_METHOD(int, FastInit, (QByteArray output), (override));
    MOCK_METHOD(int, SetLecLines, (int lec1, int lec2), (override));
    MOCK_METHOD(int, PulseLec1Line, (int timeout), (override));
    MOCK_METHOD(int, PulseLec2Line, (int timeout), (override));
    MOCK_METHOD(void, ResetConnection, (), (override));
    MOCK_METHOD(QByteArray, ReadSerialObdData, (std::uint16_t timeout), (override));
    MOCK_METHOD(QByteArray, ReadSerialData, (std::uint16_t timeout), (override));
    MOCK_METHOD(QByteArray, WriteSerialData, (QByteArray output), (override));
    MOCK_METHOD(QByteArray, WriteSerialDataEchoCheck, (QByteArray output), (override));
    MOCK_METHOD(bool, GetIsTxDone, (), (override));
    MOCK_METHOD(int, ClearRxBuffer, (), (override));
    MOCK_METHOD(int, ClearTxBuffer, (), (override));
    MOCK_METHOD(int, SendPeriodicJ2534Data, (QByteArray output, int timeout), (override));
    MOCK_METHOD(int, StopPeriodicJ2534Data, (), (override));
    MOCK_METHOD(QStringList, CheckSerialPorts, (), (override));
    MOCK_METHOD(QString, OpenSerialPort, (), (override));
    MOCK_METHOD(unsigned long, ReadVbatt, (), (override));
    MOCK_METHOD(void, WaitForSource, (), (override));

  private:
    void InstallDefaultActions()
    {
        ON_CALL(*this, GetSerialPortAvailable())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSerialPortAvailable(); });
        ON_CALL(*this, SetSerialPortAvailable(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::SetSerialPortAvailable(value); });
        ON_CALL(*this, GetSetRequestToSend())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSetRequestToSend(); });
        ON_CALL(*this, SetSetRequestToSend(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::SetSetRequestToSend(value); });
        ON_CALL(*this, GetSetDataTerminalReady())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSetDataTerminalReady(); });
        ON_CALL(*this, SetSetDataTerminalReady(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::SetSetDataTerminalReady(value); });
        ON_CALL(*this, GetAddSsmHeader()).WillByDefault([this] { return SerialPortActionsDirect::GetAddSsmHeader(); });
        ON_CALL(*this, SetAddSsmHeader(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::SetAddSsmHeader(value); });
        ON_CALL(*this, GetAddIso9141Header())
            .WillByDefault([this] { return SerialPortActionsDirect::GetAddIso9141Header(); });
        ON_CALL(*this, SetAddIso9141Header(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::SetAddIso9141Header(value); });
        ON_CALL(*this, GetAddIso14230Header())
            .WillByDefault([this] { return SerialPortActionsDirect::GetAddIso14230Header(); });
        ON_CALL(*this, SetAddIso14230Header(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::SetAddIso14230Header(value); });
        ON_CALL(*this, GetIsIso14230Connection())
            .WillByDefault([this] { return SerialPortActionsDirect::GetIsIso14230Connection(); });
        ON_CALL(*this, SetIsIso14230Connection(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::SetIsIso14230Connection(value); });
        ON_CALL(*this, GetIsCanConnection())
            .WillByDefault([this] { return SerialPortActionsDirect::GetIsCanConnection(); });
        ON_CALL(*this, SetIsCanConnection(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::SetIsCanConnection(value); });
        ON_CALL(*this, GetIsIso15765Connection())
            .WillByDefault([this] { return SerialPortActionsDirect::GetIsIso15765Connection(); });
        ON_CALL(*this, SetIsIso15765Connection(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::SetIsIso15765Connection(value); });
        ON_CALL(*this, GetIs29BitId()).WillByDefault([this] { return SerialPortActionsDirect::GetIs29BitId(); });
        ON_CALL(*this, SetIs29BitId(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::SetIs29BitId(value); });
        ON_CALL(*this, GetUseOpenport2Adapter())
            .WillByDefault([this] { return SerialPortActionsDirect::GetUseOpenport2Adapter(); });
        ON_CALL(*this, SetUseOpenport2Adapter(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::SetUseOpenport2Adapter(value); });

        ON_CALL(*this, GetRequestToSendEnabled())
            .WillByDefault([this] { return SerialPortActionsDirect::GetRequestToSendEnabled(); });
        ON_CALL(*this, SetRequestToSendEnabled(::testing::_))
            .WillByDefault([this](int value) { return SerialPortActionsDirect::SetRequestToSendEnabled(value); });
        ON_CALL(*this, GetRequestToSendDisabled())
            .WillByDefault([this] { return SerialPortActionsDirect::GetRequestToSendDisabled(); });
        ON_CALL(*this, SetRequestToSendDisabled(::testing::_))
            .WillByDefault([this](int value) { return SerialPortActionsDirect::SetRequestToSendDisabled(value); });
        ON_CALL(*this, GetDataTerminalEnabled())
            .WillByDefault([this] { return SerialPortActionsDirect::GetDataTerminalEnabled(); });
        ON_CALL(*this, SetDataTerminalEnabled(::testing::_))
            .WillByDefault([this](int value) { return SerialPortActionsDirect::SetDataTerminalEnabled(value); });
        ON_CALL(*this, GetDataTerminalDisabled())
            .WillByDefault([this] { return SerialPortActionsDirect::GetDataTerminalDisabled(); });
        ON_CALL(*this, SetDataTerminalDisabled(::testing::_))
            .WillByDefault([this](int value) { return SerialPortActionsDirect::SetDataTerminalDisabled(value); });

        ON_CALL(*this, GetKlineStartbyte())
            .WillByDefault([this] { return SerialPortActionsDirect::GetKlineStartbyte(); });
        ON_CALL(*this, SetKlineStartbyte(::testing::_))
            .WillByDefault([this](std::uint8_t value) { return SerialPortActionsDirect::SetKlineStartbyte(value); });
        ON_CALL(*this, GetKlineTesterId())
            .WillByDefault([this] { return SerialPortActionsDirect::GetKlineTesterId(); });
        ON_CALL(*this, SetKlineTesterId(::testing::_))
            .WillByDefault([this](std::uint8_t value) { return SerialPortActionsDirect::SetKlineTesterId(value); });
        ON_CALL(*this, GetKlineTargetId())
            .WillByDefault([this] { return SerialPortActionsDirect::GetKlineTargetId(); });
        ON_CALL(*this, SetKlineTargetId(::testing::_))
            .WillByDefault([this](std::uint8_t value) { return SerialPortActionsDirect::SetKlineTargetId(value); });
        ON_CALL(*this, GetSerialPortParity())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSerialPortParity(); });
        ON_CALL(*this, SetSerialPortParity(::testing::_))
            .WillByDefault([this](std::uint8_t parity)
                           { return SerialPortActionsDirect::SetSerialPortParity(parity); });

        ON_CALL(*this, GetSsmReceiveHeaderStart())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSsmReceiveHeaderStart(); });
        ON_CALL(*this, SetSsmReceiveHeaderStart(::testing::_))
            .WillByDefault([this](QByteArray value)
                           { return SerialPortActionsDirect::SetSsmReceiveHeaderStart(value); });
        ON_CALL(*this, GetSerialPortList())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSerialPortList(); });
        ON_CALL(*this, SetSerialPortList(::testing::_))
            .WillByDefault([this](QStringList value) { return SerialPortActionsDirect::SetSerialPortList(value); });

        ON_CALL(*this, GetOpenedSerialPort())
            .WillByDefault([this] { return SerialPortActionsDirect::GetOpenedSerialPort(); });
        ON_CALL(*this, SetOpenedSerialPort(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::SetOpenedSerialPort(value); });
        ON_CALL(*this, GetSubaru0216bitBootloaderBaudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSubaru0216bitBootloaderBaudrate(); });
        ON_CALL(*this, SetSubaru0216bitBootloaderBaudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::SetSubaru0216bitBootloaderBaudrate(value); });
        ON_CALL(*this, GetSubaru0416bitBootloaderBaudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSubaru0416bitBootloaderBaudrate(); });
        ON_CALL(*this, SetSubaru0416bitBootloaderBaudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::SetSubaru0416bitBootloaderBaudrate(value); });
        ON_CALL(*this, GetSubaru0232bitBootloaderBaudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSubaru0232bitBootloaderBaudrate(); });
        ON_CALL(*this, SetSubaru0232bitBootloaderBaudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::SetSubaru0232bitBootloaderBaudrate(value); });
        ON_CALL(*this, GetSubaru0432bitBootloaderBaudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSubaru0432bitBootloaderBaudrate(); });
        ON_CALL(*this, SetSubaru0432bitBootloaderBaudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::SetSubaru0432bitBootloaderBaudrate(value); });
        ON_CALL(*this, GetSubaru0532bitBootloaderBaudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSubaru0532bitBootloaderBaudrate(); });
        ON_CALL(*this, SetSubaru0532bitBootloaderBaudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::SetSubaru0532bitBootloaderBaudrate(value); });
        ON_CALL(*this, GetSubaru0216bitKernelBaudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSubaru0216bitKernelBaudrate(); });
        ON_CALL(*this, SetSubaru0216bitKernelBaudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::SetSubaru0216bitKernelBaudrate(value); });
        ON_CALL(*this, GetSubaru0416bitKernelBaudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSubaru0416bitKernelBaudrate(); });
        ON_CALL(*this, SetSubaru0416bitKernelBaudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::SetSubaru0416bitKernelBaudrate(value); });
        ON_CALL(*this, GetSubaru0232bitKernelBaudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSubaru0232bitKernelBaudrate(); });
        ON_CALL(*this, SetSubaru0232bitKernelBaudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::SetSubaru0232bitKernelBaudrate(value); });
        ON_CALL(*this, GetSubaru0432bitKernelBaudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSubaru0432bitKernelBaudrate(); });
        ON_CALL(*this, SetSubaru0432bitKernelBaudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::SetSubaru0432bitKernelBaudrate(value); });
        ON_CALL(*this, GetSubaru0532bitKernelBaudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSubaru0532bitKernelBaudrate(); });
        ON_CALL(*this, SetSubaru0532bitKernelBaudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::SetSubaru0532bitKernelBaudrate(value); });
        ON_CALL(*this, GetCanSpeed()).WillByDefault([this] { return SerialPortActionsDirect::GetCanSpeed(); });
        ON_CALL(*this, SetCanSpeed(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::SetCanSpeed(value); });
        ON_CALL(*this, GetSerialPortBaudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSerialPortBaudrate(); });
        ON_CALL(*this, SetSerialPortBaudrate(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::SetSerialPortBaudrate(value); });
        ON_CALL(*this, GetSerialPortLinux())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSerialPortLinux(); });
        ON_CALL(*this, SetSerialPortLinux(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::SetSerialPortLinux(value); });
        ON_CALL(*this, GetSerialPortWindows())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSerialPortWindows(); });
        ON_CALL(*this, SetSerialPortWindows(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::SetSerialPortWindows(value); });
        ON_CALL(*this, GetSerialPort()).WillByDefault([this] { return SerialPortActionsDirect::GetSerialPort(); });
        ON_CALL(*this, SetSerialPort(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::SetSerialPort(value); });
        ON_CALL(*this, GetSerialPortPrefix())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSerialPortPrefix(); });
        ON_CALL(*this, SetSerialPortPrefix(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::SetSerialPortPrefix(value); });
        ON_CALL(*this, GetSerialPortPrefixLinux())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSerialPortPrefixLinux(); });
        ON_CALL(*this, SetSerialPortPrefixLinux(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::SetSerialPortPrefixLinux(value); });
        ON_CALL(*this, GetSerialPortPrefixWin())
            .WillByDefault([this] { return SerialPortActionsDirect::GetSerialPortPrefixWin(); });
        ON_CALL(*this, SetSerialPortPrefixWin(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::SetSerialPortPrefixWin(value); });

        ON_CALL(*this, GetCanSourceAddress())
            .WillByDefault([this] { return SerialPortActionsDirect::GetCanSourceAddress(); });
        ON_CALL(*this, SetCanSourceAddress(::testing::_))
            .WillByDefault([this](std::uint32_t value) { return SerialPortActionsDirect::SetCanSourceAddress(value); });
        ON_CALL(*this, GetCanDestinationAddress())
            .WillByDefault([this] { return SerialPortActionsDirect::GetCanDestinationAddress(); });
        ON_CALL(*this, SetCanDestinationAddress(::testing::_))
            .WillByDefault([this](std::uint32_t value)
                           { return SerialPortActionsDirect::SetCanDestinationAddress(value); });
        ON_CALL(*this, GetIso15765SourceAddress())
            .WillByDefault([this] { return SerialPortActionsDirect::GetIso15765SourceAddress(); });
        ON_CALL(*this, SetIso15765SourceAddress(::testing::_))
            .WillByDefault([this](std::uint32_t value)
                           { return SerialPortActionsDirect::SetIso15765SourceAddress(value); });
        ON_CALL(*this, GetIso15765DestinationAddress())
            .WillByDefault([this] { return SerialPortActionsDirect::GetIso15765DestinationAddress(); });
        ON_CALL(*this, SetIso15765DestinationAddress(::testing::_))
            .WillByDefault([this](std::uint32_t value)
                           { return SerialPortActionsDirect::SetIso15765DestinationAddress(value); });

        ON_CALL(*this, IsSerialPortOpen()).WillByDefault(::testing::Return(true));
        ON_CALL(*this, ChangePortSpeed(::testing::_)).WillByDefault(::testing::Return(kSerialSuccess));
        ON_CALL(*this, SetKlineTimings(::testing::_, ::testing::_))
            .WillByDefault([this](std::uint32_t parameter, int value)
                           { return SerialPortActionsDirect::SetKlineTimings(parameter, value); });
        ON_CALL(*this, SetJ2534Ioctl(::testing::_, ::testing::_)).WillByDefault(::testing::Return(kSerialSuccess));
        ON_CALL(*this, FiveBaudInit(::testing::_)).WillByDefault(::testing::Return(QByteArray{}));
        ON_CALL(*this, FastInit(::testing::_)).WillByDefault(::testing::Return(kSerialSuccess));
        ON_CALL(*this, SetLecLines(::testing::_, ::testing::_)).WillByDefault(::testing::Return(kSerialSuccess));
        ON_CALL(*this, PulseLec1Line(::testing::_)).WillByDefault(::testing::Return(kSerialSuccess));
        ON_CALL(*this, PulseLec2Line(::testing::_)).WillByDefault(::testing::Return(kSerialSuccess));
        ON_CALL(*this, ResetConnection()).WillByDefault([] {});
        ON_CALL(*this, ReadSerialObdData(::testing::_)).WillByDefault(::testing::Return(QByteArray{}));
        ON_CALL(*this, ReadSerialData(::testing::_)).WillByDefault(::testing::Return(QByteArray{}));
        ON_CALL(*this, WriteSerialData(::testing::_)).WillByDefault(::testing::Return(QByteArray{}));
        ON_CALL(*this, WriteSerialDataEchoCheck(::testing::_)).WillByDefault(::testing::Return(QByteArray{}));
        ON_CALL(*this, GetIsTxDone()).WillByDefault(::testing::Return(true));
        ON_CALL(*this, ClearRxBuffer()).WillByDefault(::testing::Return(kSerialSuccess));
        ON_CALL(*this, ClearTxBuffer()).WillByDefault(::testing::Return(kSerialSuccess));
        ON_CALL(*this, SendPeriodicJ2534Data(::testing::_, ::testing::_))
            .WillByDefault(::testing::Return(kSerialSuccess));
        ON_CALL(*this, StopPeriodicJ2534Data()).WillByDefault(::testing::Return(kSerialSuccess));
        ON_CALL(*this, CheckSerialPorts()).WillByDefault(::testing::Return(QStringList{}));
        ON_CALL(*this, OpenSerialPort()).WillByDefault(::testing::Return(QString{}));
        ON_CALL(*this, ReadVbatt()).WillByDefault(::testing::Return(0UL));
        ON_CALL(*this, WaitForSource()).WillByDefault([] {});
    }
};

using NiceFakeBackend = ::testing::NiceMock<FakeBackend>;
