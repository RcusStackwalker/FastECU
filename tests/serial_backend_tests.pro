# Backend/adapter-level and thread-safety tests for the serial backend
# decoupling (docs/superpowers/specs/2026-07-02-serial-backend-decoupling-design.md).
# Run with: qmake6 serial_backend_tests.pro && make && ./serial_backend_tests
QT += core testlib serialport remoteobjects websockets widgets
CONFIG += c++17 console
CONFIG -= app_bundle
include(../hardening.pri)

TARGET = serial_backend_tests
INCLUDEPATH += .. ../serial_port

REPC_REPLICA = ../serial_port/serial_port_actions.rep

unix {
    SOURCES += ../serial_port/J2534_unix.cpp
    HEADERS += ../serial_port/J2534_unix.h
}

SOURCES += \
    serial_backend_main.cpp \
    test_direct_backend.cpp \
    ../serial_port/serial_port_actions_direct.cpp

HEADERS += \
    mock_openport.h \
    test_direct_backend.h \
    ../serial_port/serial_backend.h \
    ../serial_port/serial_port_actions_direct.h
