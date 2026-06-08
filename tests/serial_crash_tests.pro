# Headless regression tests reproducing the OpenPort/J2534 read-path crash on
# macOS (EXC_BAD_ACCESS at 0x8 in QIODevice::isOpen()). Run with:
#   qmake6 serial_crash_tests.pro && make && ./serial_crash_tests
# Pre-fix each test reproduces the crash (SIGSEGV at 0x8); post-fix they pass.

QT += core testlib serialport widgets
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = serial_crash_tests

INCLUDEPATH += .. ../serial_port

SOURCES += \
    tst_serial_port_crash.cpp \
    ../serial_port/J2534_unix.cpp \
    ../serial_port/serial_port_actions_direct.cpp

HEADERS += \
    ../serial_port/J2534_unix.h \
    ../serial_port/serial_port_actions_direct.h
