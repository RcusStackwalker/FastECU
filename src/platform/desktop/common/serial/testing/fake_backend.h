#pragma once

#include <gmock/gmock.h>

#include <cstdint>

#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

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
// guarantees, how to set expectations against it, and the QtTest integration it
// requires are in docs/gmock-reference.md.
class FakeBackend : public SerialPortActionsDirect
{
    Q_OBJECT

  public:
    FakeBackend()
    {
        installDefaultActions();
    }

    ~FakeBackend() override
    {
        if (destroyed)
        {
            *destroyed = true;
        }
    }

    bool *destroyed = nullptr;

    MOCK_METHOD(bool, get_serialPortAvailable, (), (override));
    MOCK_METHOD(bool, set_serialPortAvailable, (bool value), (override));
    MOCK_METHOD(bool, get_setRequestToSend, (), (override));
    MOCK_METHOD(bool, set_setRequestToSend, (bool value), (override));
    MOCK_METHOD(bool, get_setDataTerminalReady, (), (override));
    MOCK_METHOD(bool, set_setDataTerminalReady, (bool value), (override));
    MOCK_METHOD(bool, get_add_ssm_header, (), (override));
    MOCK_METHOD(bool, set_add_ssm_header, (bool value), (override));
    MOCK_METHOD(bool, get_add_iso9141_header, (), (override));
    MOCK_METHOD(bool, set_add_iso9141_header, (bool value), (override));
    MOCK_METHOD(bool, get_add_iso14230_header, (), (override));
    MOCK_METHOD(bool, set_add_iso14230_header, (bool value), (override));
    MOCK_METHOD(bool, get_is_iso14230_connection, (), (override));
    MOCK_METHOD(bool, set_is_iso14230_connection, (bool value), (override));
    MOCK_METHOD(bool, get_is_can_connection, (), (override));
    MOCK_METHOD(bool, set_is_can_connection, (bool value), (override));
    MOCK_METHOD(bool, get_is_iso15765_connection, (), (override));
    MOCK_METHOD(bool, set_is_iso15765_connection, (bool value), (override));
    MOCK_METHOD(bool, get_is_29_bit_id, (), (override));
    MOCK_METHOD(bool, set_is_29_bit_id, (bool value), (override));
    MOCK_METHOD(bool, get_use_openport2_adapter, (), (override));
    MOCK_METHOD(bool, set_use_openport2_adapter, (bool value), (override));

    MOCK_METHOD(int, get_requestToSendEnabled, (), (override));
    MOCK_METHOD(bool, set_requestToSendEnabled, (int value), (override));
    MOCK_METHOD(int, get_requestToSendDisabled, (), (override));
    MOCK_METHOD(bool, set_requestToSendDisabled, (int value), (override));
    MOCK_METHOD(int, get_dataTerminalEnabled, (), (override));
    MOCK_METHOD(bool, set_dataTerminalEnabled, (int value), (override));
    MOCK_METHOD(int, get_dataTerminalDisabled, (), (override));
    MOCK_METHOD(bool, set_dataTerminalDisabled, (int value), (override));

    MOCK_METHOD(std::uint8_t, get_kline_startbyte, (), (override));
    MOCK_METHOD(bool, set_kline_startbyte, (std::uint8_t value), (override));
    MOCK_METHOD(std::uint8_t, get_kline_tester_id, (), (override));
    MOCK_METHOD(bool, set_kline_tester_id, (std::uint8_t value), (override));
    MOCK_METHOD(std::uint8_t, get_kline_target_id, (), (override));
    MOCK_METHOD(bool, set_kline_target_id, (std::uint8_t value), (override));
    MOCK_METHOD(std::uint8_t, get_serial_port_parity, (), (override));
    MOCK_METHOD(bool, set_serial_port_parity, (std::uint8_t parity), (override));

    MOCK_METHOD(QByteArray, get_ssm_receive_header_start, (), (override));
    MOCK_METHOD(bool, set_ssm_receive_header_start, (QByteArray value), (override));

    MOCK_METHOD(QStringList, get_serial_port_list, (), (override));
    MOCK_METHOD(bool, set_serial_port_list, (QStringList value), (override));

    MOCK_METHOD(QString, get_openedSerialPort, (), (override));
    MOCK_METHOD(bool, set_openedSerialPort, (QString value), (override));
    MOCK_METHOD(QString, get_subaru_02_16bit_bootloader_baudrate, (), (override));
    MOCK_METHOD(bool, set_subaru_02_16bit_bootloader_baudrate, (QString value), (override));
    MOCK_METHOD(QString, get_subaru_04_16bit_bootloader_baudrate, (), (override));
    MOCK_METHOD(bool, set_subaru_04_16bit_bootloader_baudrate, (QString value), (override));
    MOCK_METHOD(QString, get_subaru_02_32bit_bootloader_baudrate, (), (override));
    MOCK_METHOD(bool, set_subaru_02_32bit_bootloader_baudrate, (QString value), (override));
    MOCK_METHOD(QString, get_subaru_04_32bit_bootloader_baudrate, (), (override));
    MOCK_METHOD(bool, set_subaru_04_32bit_bootloader_baudrate, (QString value), (override));
    MOCK_METHOD(QString, get_subaru_05_32bit_bootloader_baudrate, (), (override));
    MOCK_METHOD(bool, set_subaru_05_32bit_bootloader_baudrate, (QString value), (override));
    MOCK_METHOD(QString, get_subaru_02_16bit_kernel_baudrate, (), (override));
    MOCK_METHOD(bool, set_subaru_02_16bit_kernel_baudrate, (QString value), (override));
    MOCK_METHOD(QString, get_subaru_04_16bit_kernel_baudrate, (), (override));
    MOCK_METHOD(bool, set_subaru_04_16bit_kernel_baudrate, (QString value), (override));
    MOCK_METHOD(QString, get_subaru_02_32bit_kernel_baudrate, (), (override));
    MOCK_METHOD(bool, set_subaru_02_32bit_kernel_baudrate, (QString value), (override));
    MOCK_METHOD(QString, get_subaru_04_32bit_kernel_baudrate, (), (override));
    MOCK_METHOD(bool, set_subaru_04_32bit_kernel_baudrate, (QString value), (override));
    MOCK_METHOD(QString, get_subaru_05_32bit_kernel_baudrate, (), (override));
    MOCK_METHOD(bool, set_subaru_05_32bit_kernel_baudrate, (QString value), (override));
    MOCK_METHOD(QString, get_can_speed, (), (override));
    MOCK_METHOD(bool, set_can_speed, (QString value), (override));
    MOCK_METHOD(QString, get_serial_port_baudrate, (), (override));
    MOCK_METHOD(bool, set_serial_port_baudrate, (QString value), (override));
    MOCK_METHOD(QString, get_serial_port_linux, (), (override));
    MOCK_METHOD(bool, set_serial_port_linux, (QString value), (override));
    MOCK_METHOD(QString, get_serial_port_windows, (), (override));
    MOCK_METHOD(bool, set_serial_port_windows, (QString value), (override));
    MOCK_METHOD(QString, get_serial_port, (), (override));
    MOCK_METHOD(bool, set_serial_port, (QString value), (override));
    MOCK_METHOD(QString, get_serial_port_prefix, (), (override));
    MOCK_METHOD(bool, set_serial_port_prefix, (QString value), (override));
    MOCK_METHOD(QString, get_serial_port_prefix_linux, (), (override));
    MOCK_METHOD(bool, set_serial_port_prefix_linux, (QString value), (override));
    MOCK_METHOD(QString, get_serial_port_prefix_win, (), (override));
    MOCK_METHOD(bool, set_serial_port_prefix_win, (QString value), (override));

    MOCK_METHOD(std::uint32_t, get_can_source_address, (), (override));
    MOCK_METHOD(bool, set_can_source_address, (std::uint32_t value), (override));
    MOCK_METHOD(std::uint32_t, get_can_destination_address, (), (override));
    MOCK_METHOD(bool, set_can_destination_address, (std::uint32_t value), (override));
    MOCK_METHOD(std::uint32_t, get_iso15765_source_address, (), (override));
    MOCK_METHOD(bool, set_iso15765_source_address, (std::uint32_t value), (override));
    MOCK_METHOD(std::uint32_t, get_iso15765_destination_address, (), (override));
    MOCK_METHOD(bool, set_iso15765_destination_address, (std::uint32_t value), (override));

    MOCK_METHOD(bool, is_serial_port_open, (), (override));
    MOCK_METHOD(int, change_port_speed, (QString portSpeed), (override));
    MOCK_METHOD(bool, set_kline_timings, (std::uint32_t parameter, int value), (override));
    MOCK_METHOD(int, set_j2534_ioctl, (std::uint32_t parameter, int value), (override));
    MOCK_METHOD(QByteArray, five_baud_init, (QByteArray output), (override));
    MOCK_METHOD(int, fast_init, (QByteArray output), (override));
    MOCK_METHOD(int, set_lec_lines, (int lec1, int lec2), (override));
    MOCK_METHOD(int, pulse_lec_1_line, (int timeout), (override));
    MOCK_METHOD(int, pulse_lec_2_line, (int timeout), (override));
    MOCK_METHOD(void, reset_connection, (), (override));
    MOCK_METHOD(QByteArray, read_serial_obd_data, (std::uint16_t timeout), (override));
    MOCK_METHOD(QByteArray, read_serial_data, (std::uint16_t timeout), (override));
    MOCK_METHOD(QByteArray, write_serial_data, (QByteArray output), (override));
    MOCK_METHOD(QByteArray, write_serial_data_echo_check, (QByteArray output), (override));
    MOCK_METHOD(bool, get_is_tx_done, (), (override));
    MOCK_METHOD(int, clear_rx_buffer, (), (override));
    MOCK_METHOD(int, clear_tx_buffer, (), (override));
    MOCK_METHOD(int, send_periodic_j2534_data, (QByteArray output, int timeout), (override));
    MOCK_METHOD(int, stop_periodic_j2534_data, (), (override));
    MOCK_METHOD(QStringList, check_serial_ports, (), (override));
    MOCK_METHOD(QString, open_serial_port, (), (override));
    MOCK_METHOD(unsigned long, read_vbatt, (), (override));
    MOCK_METHOD(void, waitForSource, (), (override));

  private:
    void installDefaultActions()
    {
        ON_CALL(*this, get_serialPortAvailable())
            .WillByDefault([this] { return SerialPortActionsDirect::get_serialPortAvailable(); });
        ON_CALL(*this, set_serialPortAvailable(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::set_serialPortAvailable(value); });
        ON_CALL(*this, get_setRequestToSend())
            .WillByDefault([this] { return SerialPortActionsDirect::get_setRequestToSend(); });
        ON_CALL(*this, set_setRequestToSend(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::set_setRequestToSend(value); });
        ON_CALL(*this, get_setDataTerminalReady())
            .WillByDefault([this] { return SerialPortActionsDirect::get_setDataTerminalReady(); });
        ON_CALL(*this, set_setDataTerminalReady(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::set_setDataTerminalReady(value); });
        ON_CALL(*this, get_add_ssm_header())
            .WillByDefault([this] { return SerialPortActionsDirect::get_add_ssm_header(); });
        ON_CALL(*this, set_add_ssm_header(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::set_add_ssm_header(value); });
        ON_CALL(*this, get_add_iso9141_header())
            .WillByDefault([this] { return SerialPortActionsDirect::get_add_iso9141_header(); });
        ON_CALL(*this, set_add_iso9141_header(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::set_add_iso9141_header(value); });
        ON_CALL(*this, get_add_iso14230_header())
            .WillByDefault([this] { return SerialPortActionsDirect::get_add_iso14230_header(); });
        ON_CALL(*this, set_add_iso14230_header(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::set_add_iso14230_header(value); });
        ON_CALL(*this, get_is_iso14230_connection())
            .WillByDefault([this] { return SerialPortActionsDirect::get_is_iso14230_connection(); });
        ON_CALL(*this, set_is_iso14230_connection(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::set_is_iso14230_connection(value); });
        ON_CALL(*this, get_is_can_connection())
            .WillByDefault([this] { return SerialPortActionsDirect::get_is_can_connection(); });
        ON_CALL(*this, set_is_can_connection(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::set_is_can_connection(value); });
        ON_CALL(*this, get_is_iso15765_connection())
            .WillByDefault([this] { return SerialPortActionsDirect::get_is_iso15765_connection(); });
        ON_CALL(*this, set_is_iso15765_connection(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::set_is_iso15765_connection(value); });
        ON_CALL(*this, get_is_29_bit_id())
            .WillByDefault([this] { return SerialPortActionsDirect::get_is_29_bit_id(); });
        ON_CALL(*this, set_is_29_bit_id(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::set_is_29_bit_id(value); });
        ON_CALL(*this, get_use_openport2_adapter())
            .WillByDefault([this] { return SerialPortActionsDirect::get_use_openport2_adapter(); });
        ON_CALL(*this, set_use_openport2_adapter(::testing::_))
            .WillByDefault([this](bool value) { return SerialPortActionsDirect::set_use_openport2_adapter(value); });

        ON_CALL(*this, get_requestToSendEnabled())
            .WillByDefault([this] { return SerialPortActionsDirect::get_requestToSendEnabled(); });
        ON_CALL(*this, set_requestToSendEnabled(::testing::_))
            .WillByDefault([this](int value) { return SerialPortActionsDirect::set_requestToSendEnabled(value); });
        ON_CALL(*this, get_requestToSendDisabled())
            .WillByDefault([this] { return SerialPortActionsDirect::get_requestToSendDisabled(); });
        ON_CALL(*this, set_requestToSendDisabled(::testing::_))
            .WillByDefault([this](int value) { return SerialPortActionsDirect::set_requestToSendDisabled(value); });
        ON_CALL(*this, get_dataTerminalEnabled())
            .WillByDefault([this] { return SerialPortActionsDirect::get_dataTerminalEnabled(); });
        ON_CALL(*this, set_dataTerminalEnabled(::testing::_))
            .WillByDefault([this](int value) { return SerialPortActionsDirect::set_dataTerminalEnabled(value); });
        ON_CALL(*this, get_dataTerminalDisabled())
            .WillByDefault([this] { return SerialPortActionsDirect::get_dataTerminalDisabled(); });
        ON_CALL(*this, set_dataTerminalDisabled(::testing::_))
            .WillByDefault([this](int value) { return SerialPortActionsDirect::set_dataTerminalDisabled(value); });

        ON_CALL(*this, get_kline_startbyte())
            .WillByDefault([this] { return SerialPortActionsDirect::get_kline_startbyte(); });
        ON_CALL(*this, set_kline_startbyte(::testing::_))
            .WillByDefault([this](std::uint8_t value) { return SerialPortActionsDirect::set_kline_startbyte(value); });
        ON_CALL(*this, get_kline_tester_id())
            .WillByDefault([this] { return SerialPortActionsDirect::get_kline_tester_id(); });
        ON_CALL(*this, set_kline_tester_id(::testing::_))
            .WillByDefault([this](std::uint8_t value) { return SerialPortActionsDirect::set_kline_tester_id(value); });
        ON_CALL(*this, get_kline_target_id())
            .WillByDefault([this] { return SerialPortActionsDirect::get_kline_target_id(); });
        ON_CALL(*this, set_kline_target_id(::testing::_))
            .WillByDefault([this](std::uint8_t value) { return SerialPortActionsDirect::set_kline_target_id(value); });
        ON_CALL(*this, get_serial_port_parity())
            .WillByDefault([this] { return SerialPortActionsDirect::get_serial_port_parity(); });
        ON_CALL(*this, set_serial_port_parity(::testing::_))
            .WillByDefault([this](std::uint8_t parity)
                           { return SerialPortActionsDirect::set_serial_port_parity(parity); });

        ON_CALL(*this, get_ssm_receive_header_start())
            .WillByDefault([this] { return SerialPortActionsDirect::get_ssm_receive_header_start(); });
        ON_CALL(*this, set_ssm_receive_header_start(::testing::_))
            .WillByDefault([this](QByteArray value)
                           { return SerialPortActionsDirect::set_ssm_receive_header_start(value); });
        ON_CALL(*this, get_serial_port_list())
            .WillByDefault([this] { return SerialPortActionsDirect::get_serial_port_list(); });
        ON_CALL(*this, set_serial_port_list(::testing::_))
            .WillByDefault([this](QStringList value) { return SerialPortActionsDirect::set_serial_port_list(value); });

        ON_CALL(*this, get_openedSerialPort())
            .WillByDefault([this] { return SerialPortActionsDirect::get_openedSerialPort(); });
        ON_CALL(*this, set_openedSerialPort(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::set_openedSerialPort(value); });
        ON_CALL(*this, get_subaru_02_16bit_bootloader_baudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::get_subaru_02_16bit_bootloader_baudrate(); });
        ON_CALL(*this, set_subaru_02_16bit_bootloader_baudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_subaru_02_16bit_bootloader_baudrate(value); });
        ON_CALL(*this, get_subaru_04_16bit_bootloader_baudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::get_subaru_04_16bit_bootloader_baudrate(); });
        ON_CALL(*this, set_subaru_04_16bit_bootloader_baudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_subaru_04_16bit_bootloader_baudrate(value); });
        ON_CALL(*this, get_subaru_02_32bit_bootloader_baudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::get_subaru_02_32bit_bootloader_baudrate(); });
        ON_CALL(*this, set_subaru_02_32bit_bootloader_baudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_subaru_02_32bit_bootloader_baudrate(value); });
        ON_CALL(*this, get_subaru_04_32bit_bootloader_baudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::get_subaru_04_32bit_bootloader_baudrate(); });
        ON_CALL(*this, set_subaru_04_32bit_bootloader_baudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_subaru_04_32bit_bootloader_baudrate(value); });
        ON_CALL(*this, get_subaru_05_32bit_bootloader_baudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::get_subaru_05_32bit_bootloader_baudrate(); });
        ON_CALL(*this, set_subaru_05_32bit_bootloader_baudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_subaru_05_32bit_bootloader_baudrate(value); });
        ON_CALL(*this, get_subaru_02_16bit_kernel_baudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::get_subaru_02_16bit_kernel_baudrate(); });
        ON_CALL(*this, set_subaru_02_16bit_kernel_baudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_subaru_02_16bit_kernel_baudrate(value); });
        ON_CALL(*this, get_subaru_04_16bit_kernel_baudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::get_subaru_04_16bit_kernel_baudrate(); });
        ON_CALL(*this, set_subaru_04_16bit_kernel_baudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_subaru_04_16bit_kernel_baudrate(value); });
        ON_CALL(*this, get_subaru_02_32bit_kernel_baudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::get_subaru_02_32bit_kernel_baudrate(); });
        ON_CALL(*this, set_subaru_02_32bit_kernel_baudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_subaru_02_32bit_kernel_baudrate(value); });
        ON_CALL(*this, get_subaru_04_32bit_kernel_baudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::get_subaru_04_32bit_kernel_baudrate(); });
        ON_CALL(*this, set_subaru_04_32bit_kernel_baudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_subaru_04_32bit_kernel_baudrate(value); });
        ON_CALL(*this, get_subaru_05_32bit_kernel_baudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::get_subaru_05_32bit_kernel_baudrate(); });
        ON_CALL(*this, set_subaru_05_32bit_kernel_baudrate(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_subaru_05_32bit_kernel_baudrate(value); });
        ON_CALL(*this, get_can_speed()).WillByDefault([this] { return SerialPortActionsDirect::get_can_speed(); });
        ON_CALL(*this, set_can_speed(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::set_can_speed(value); });
        ON_CALL(*this, get_serial_port_baudrate())
            .WillByDefault([this] { return SerialPortActionsDirect::get_serial_port_baudrate(); });
        ON_CALL(*this, set_serial_port_baudrate(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::set_serial_port_baudrate(value); });
        ON_CALL(*this, get_serial_port_linux())
            .WillByDefault([this] { return SerialPortActionsDirect::get_serial_port_linux(); });
        ON_CALL(*this, set_serial_port_linux(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::set_serial_port_linux(value); });
        ON_CALL(*this, get_serial_port_windows())
            .WillByDefault([this] { return SerialPortActionsDirect::get_serial_port_windows(); });
        ON_CALL(*this, set_serial_port_windows(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::set_serial_port_windows(value); });
        ON_CALL(*this, get_serial_port()).WillByDefault([this] { return SerialPortActionsDirect::get_serial_port(); });
        ON_CALL(*this, set_serial_port(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::set_serial_port(value); });
        ON_CALL(*this, get_serial_port_prefix())
            .WillByDefault([this] { return SerialPortActionsDirect::get_serial_port_prefix(); });
        ON_CALL(*this, set_serial_port_prefix(::testing::_))
            .WillByDefault([this](QString value) { return SerialPortActionsDirect::set_serial_port_prefix(value); });
        ON_CALL(*this, get_serial_port_prefix_linux())
            .WillByDefault([this] { return SerialPortActionsDirect::get_serial_port_prefix_linux(); });
        ON_CALL(*this, set_serial_port_prefix_linux(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_serial_port_prefix_linux(value); });
        ON_CALL(*this, get_serial_port_prefix_win())
            .WillByDefault([this] { return SerialPortActionsDirect::get_serial_port_prefix_win(); });
        ON_CALL(*this, set_serial_port_prefix_win(::testing::_))
            .WillByDefault([this](QString value)
                           { return SerialPortActionsDirect::set_serial_port_prefix_win(value); });

        ON_CALL(*this, get_can_source_address())
            .WillByDefault([this] { return SerialPortActionsDirect::get_can_source_address(); });
        ON_CALL(*this, set_can_source_address(::testing::_))
            .WillByDefault([this](std::uint32_t value)
                           { return SerialPortActionsDirect::set_can_source_address(value); });
        ON_CALL(*this, get_can_destination_address())
            .WillByDefault([this] { return SerialPortActionsDirect::get_can_destination_address(); });
        ON_CALL(*this, set_can_destination_address(::testing::_))
            .WillByDefault([this](std::uint32_t value)
                           { return SerialPortActionsDirect::set_can_destination_address(value); });
        ON_CALL(*this, get_iso15765_source_address())
            .WillByDefault([this] { return SerialPortActionsDirect::get_iso15765_source_address(); });
        ON_CALL(*this, set_iso15765_source_address(::testing::_))
            .WillByDefault([this](std::uint32_t value)
                           { return SerialPortActionsDirect::set_iso15765_source_address(value); });
        ON_CALL(*this, get_iso15765_destination_address())
            .WillByDefault([this] { return SerialPortActionsDirect::get_iso15765_destination_address(); });
        ON_CALL(*this, set_iso15765_destination_address(::testing::_))
            .WillByDefault([this](std::uint32_t value)
                           { return SerialPortActionsDirect::set_iso15765_destination_address(value); });

        ON_CALL(*this, is_serial_port_open()).WillByDefault(::testing::Return(true));
        ON_CALL(*this, change_port_speed(::testing::_)).WillByDefault(::testing::Return(STATUS_SUCCESS));
        ON_CALL(*this, set_kline_timings(::testing::_, ::testing::_))
            .WillByDefault([this](std::uint32_t parameter, int value)
                           { return SerialPortActionsDirect::set_kline_timings(parameter, value); });
        ON_CALL(*this, set_j2534_ioctl(::testing::_, ::testing::_)).WillByDefault(::testing::Return(STATUS_SUCCESS));
        ON_CALL(*this, five_baud_init(::testing::_)).WillByDefault(::testing::Return(QByteArray{}));
        ON_CALL(*this, fast_init(::testing::_)).WillByDefault(::testing::Return(STATUS_SUCCESS));
        ON_CALL(*this, set_lec_lines(::testing::_, ::testing::_)).WillByDefault(::testing::Return(STATUS_SUCCESS));
        ON_CALL(*this, pulse_lec_1_line(::testing::_)).WillByDefault(::testing::Return(STATUS_SUCCESS));
        ON_CALL(*this, pulse_lec_2_line(::testing::_)).WillByDefault(::testing::Return(STATUS_SUCCESS));
        ON_CALL(*this, reset_connection()).WillByDefault([] {});
        ON_CALL(*this, read_serial_obd_data(::testing::_)).WillByDefault(::testing::Return(QByteArray{}));
        ON_CALL(*this, read_serial_data(::testing::_)).WillByDefault(::testing::Return(QByteArray{}));
        ON_CALL(*this, write_serial_data(::testing::_)).WillByDefault(::testing::Return(QByteArray{}));
        ON_CALL(*this, write_serial_data_echo_check(::testing::_)).WillByDefault(::testing::Return(QByteArray{}));
        ON_CALL(*this, get_is_tx_done()).WillByDefault(::testing::Return(true));
        ON_CALL(*this, clear_rx_buffer()).WillByDefault(::testing::Return(STATUS_SUCCESS));
        ON_CALL(*this, clear_tx_buffer()).WillByDefault(::testing::Return(STATUS_SUCCESS));
        ON_CALL(*this, send_periodic_j2534_data(::testing::_, ::testing::_))
            .WillByDefault(::testing::Return(STATUS_SUCCESS));
        ON_CALL(*this, stop_periodic_j2534_data()).WillByDefault(::testing::Return(STATUS_SUCCESS));
        ON_CALL(*this, check_serial_ports()).WillByDefault(::testing::Return(QStringList{}));
        ON_CALL(*this, open_serial_port()).WillByDefault(::testing::Return(QString{}));
        ON_CALL(*this, read_vbatt()).WillByDefault(::testing::Return(0UL));
        ON_CALL(*this, waitForSource()).WillByDefault([] {});
    }
};

using NiceFakeBackend = ::testing::NiceMock<FakeBackend>;
