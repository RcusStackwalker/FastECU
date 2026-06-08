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
    ../protocol/mut_dma_codec.cpp
HEADERS += \
    test_codec.h
