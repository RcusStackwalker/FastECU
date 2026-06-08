QT += core testlib
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle
TEMPLATE = app
TARGET = mut_dma_tests
INCLUDEPATH += $$PWD/..
SOURCES += \
    main.cpp \
    test_codec.cpp \
    test_freeform.cpp \
    test_memory.cpp \
    test_transport.cpp \
    ../protocol/mut_dma_codec.cpp \
    ../protocol/mut_dma_freeform.cpp \
    ../protocol/mut_dma_memory.cpp
HEADERS += \
    test_codec.h \
    test_freeform.h \
    test_memory.h \
    test_transport.h \
    scripted_kline_transport.h \
    ../protocol/ikline_transport.h
