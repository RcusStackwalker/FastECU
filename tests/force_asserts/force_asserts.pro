QT += core testlib
QT -= gui
CONFIG += console c++20 release
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_force_asserts
# Simulate the shipped release build: asserts stripped.
DEFINES += QT_NO_DEBUG QT_FORCE_ASSERTS
SOURCES += tst_force_asserts.cpp
