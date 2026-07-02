# Integration tests for the MUT/DMA host client wired through FastECU's REAL
# serial stack (FastEcuKlineTransport -> SerialPortActions -> SerialPortActionsDirect
# -> J2534_unix) talking to a scripted mock Openport 2.0 over a PTY.
#
# These exercise the one part the unit suite (tests.pro, ScriptedKlineTransport)
# deliberately stubs out: the FastEcuKlineTransport adapter over the real
# SerialPortActions facade. That was untestable until the macOS build landed
# (the facade pulls in the whole app: remoteobjects/websockets/widgets/serialport).
#
# Run:  qmake6 mut_dma_integration_tests.pro && make && ./mut_dma_integration_tests
QT += core testlib serialport widgets remoteobjects websockets
CONFIG += c++17 console
CONFIG -= app_bundle
include(../hardening.pri)

TARGET = mut_dma_integration_tests

INCLUDEPATH += .. ../serial_port ../protocol

REPC_REPLICA = \
    ../serial_port/serial_port_actions.rep \
    ../remote_utility/remote_utility.rep

SOURCES += \
    tst_mut_dma_integration.cpp \
    ../serial_port/serial_port_actions.cpp \
    ../serial_port/serial_port_actions_direct.cpp \
    ../serial_port/remote_serial_backend.cpp \
    ../serial_port/serial_backend_host.cpp \
    ../serial_port/J2534_unix.cpp \
    ../serial_port/websocketiodevice.cpp \
    ../protocol/fastecu_kline_transport.cpp \
    ../protocol/mut_dma_codec.cpp \
    ../protocol/mut_dma_freeform.cpp \
    ../protocol/mut_dma_memory.cpp \
    ../protocol/imut_dma_init.cpp \
    ../protocol/mut_dma_driver.cpp

HEADERS += \
    ../serial_port/serial_port_actions.h \
    ../serial_port/serial_port_actions_direct.h \
    ../serial_port/serial_backend.h \
    ../serial_port/remote_serial_backend.h \
    ../serial_port/serial_backend_host.h \
    ../serial_port/J2534_unix.h \
    ../serial_port/websocketiodevice.h
