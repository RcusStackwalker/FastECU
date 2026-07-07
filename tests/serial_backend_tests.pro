# Backend/adapter-level and thread-safety tests for the serial backend decoupling.
# Run with: qmake6 serial_backend_tests.pro && make && ./serial_backend_tests
QT += core testlib serialport remoteobjects websockets widgets
CONFIG += c++20 console
CONFIG -= app_bundle
include(../hardening.pri)

TARGET = serial_backend_tests
INCLUDEPATH += .. ../serial_port

REPC_REPLICA = ../serial_port/serial_port_actions.rep

unix {
    SOURCES += ../serial_port/J2534_unix.cpp
    HEADERS += ../serial_port/J2534_unix.h

    SOURCES += test_pty_e2e.cpp
    HEADERS += test_pty_e2e.h
}

SOURCES += \
    serial_backend_main.cpp \
    test_direct_backend.cpp \
    test_remote_backend_smoke.cpp \
    ../serial_port/serial_port_actions_direct.cpp

HEADERS += \
    mock_openport.h \
    test_direct_backend.h \
    test_remote_backend_smoke.h \
    ../serial_port/serial_backend.h \
    ../serial_port/serial_port_actions_direct.h

SOURCES += ../serial_port/remote_serial_backend.cpp \
           ../serial_port/websocketiodevice.cpp
HEADERS += ../serial_port/remote_serial_backend.h \
           ../serial_port/websocketiodevice.h

SOURCES += test_facade_threading.cpp \
           ../serial_port/serial_port_actions.cpp \
           ../serial_port/serial_backend_host.cpp
HEADERS += fake_backend.h \
           test_facade_threading.h \
           ../serial_port/serial_port_actions.h \
           ../serial_port/serial_backend_host.h
