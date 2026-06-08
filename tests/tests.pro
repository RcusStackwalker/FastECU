QT += core testlib
QT -= gui
CONFIG += console c++11 testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = mut_dma_tests
INCLUDEPATH += $$PWD/..
SOURCES += \
    test_smoke.cpp
