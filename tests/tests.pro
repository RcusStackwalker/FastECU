QT += core testlib
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle
include(../hardening.pri)
TEMPLATE = app
TARGET = mut_dma_tests
INCLUDEPATH += $$PWD/..
SOURCES += \
    main.cpp \
    test_codec.cpp \
    test_freeform.cpp \
    test_memory.cpp \
    test_transport.cpp \
    test_init.cpp \
    test_driver.cpp \
    ../protocol/mut_dma_codec.cpp \
    ../protocol/mut_dma_freeform.cpp \
    ../protocol/mut_dma_memory.cpp \
    ../protocol/imut_dma_init.cpp \
    ../protocol/mut_dma_driver.cpp
HEADERS += \
    test_codec.h \
    test_freeform.h \
    test_memory.h \
    test_transport.h \
    test_init.h \
    test_driver.h \
    scripted_kline_transport.h \
    ../protocol/ikline_transport.h
