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
    test_mitsu_colt_can_protocol.cpp \
    test_mitsu_colt_can_cdbg_protocol.cpp \
    test_cdbg_driver.cpp \
    test_logging_worker.cpp \
    test_logging_engine.cpp \
    ../protocol/mitsu_colt_can_protocol.cpp \
    ../protocol/mitsu_colt_can_cdbg_protocol.cpp \
    ../protocol/mitsu_colt_can_cdbg_driver.cpp \
    ../protocol/mut_dma_codec.cpp \
    ../protocol/mut_dma_freeform.cpp \
    ../protocol/mut_dma_memory.cpp \
    ../protocol/imut_dma_init.cpp \
    ../protocol/mut_dma_driver.cpp \
    ../logging/logging_worker.cpp \
    ../logging/logging_engine.cpp
HEADERS += \
    test_codec.h \
    test_freeform.h \
    test_memory.h \
    test_transport.h \
    test_init.h \
    test_driver.h \
    test_mitsu_colt_can_protocol.h \
    test_mitsu_colt_can_cdbg_protocol.h \
    test_cdbg_driver.h \
    test_logging_worker.h \
    test_logging_engine.h \
    scripted_kline_transport.h \
    ../protocol/ikline_transport.h \
    ../protocol/mitsu_colt_can_cdbg_protocol.h \
    ../protocol/mitsu_colt_can_cdbg_driver.h \
    scripted_can_transport.h \
    ../protocol/ican_transport.h \
    scripted_logging_protocol.h \
    ../logging/logging_protocol.h \
    ../logging/logging_worker.h \
    ../logging/logging_engine.h
