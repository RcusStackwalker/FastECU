#ifndef FAKE_BACKEND_H
#define FAKE_BACKEND_H

#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QStringList>
#include <QThread>

#include "serial_port_actions_direct.h"

// Scripted backend for facade-level tests. Subclasses the direct backend so
// the 44 config accessors come for free (real fields, no port ever opened);
// overrides the I/O entry points with scripted, observable behavior.
class FakeBackend : public SerialPortActionsDirect
{
    Q_OBJECT
public:
    int readDelayMs = 0;              // set before use; simulates a slow adapter
    QByteArray scriptedResponse;
    QSemaphore *readEntered = nullptr;
    QSemaphore *continueRead = nullptr;

    QStringList takeCallLog()
    {
        QMutexLocker l(&logMutex);
        QStringList out = callLog;
        callLog.clear();
        return out;
    }

    bool is_serial_port_open() override { return true; }

    // Real open_serial_port() unconditionally indexes serial_port_list.at(0)
    // (see serial_port_actions_direct.cpp) before doing any real hardware
    // probing -- fine for production where a port is always selected first,
    // but a test double has no port list populated. Stub it out like the
    // other I/O entry points below rather than touch real hardware/J2534.
    QString open_serial_port() override { return QString(); }

    QByteArray read_serial_data(uint16_t timeout) override
    {
        log(QString("read:begin:t=%1").arg(timeout));
        if (readEntered)
            readEntered->release();
        if (continueRead)
            continueRead->acquire();
        if (readDelayMs)
            QThread::msleep(readDelayMs);
        log("read:end");
        return scriptedResponse;
    }

    QByteArray write_serial_data(QByteArray output) override
    {
        log("write:begin:" + QString::fromLatin1(output.toHex()));
        if (readDelayMs)
            QThread::msleep(readDelayMs);
        log("write:end");
        return QByteArray();   // matches the real backend's empty return
    }

    // Distinct virtual from write_serial_data() in the backend interface --
    // the real implementation has its own body that touches the real
    // QSerialPort directly (byte-at-a-time write with echo readback), not a
    // wrapper around write_serial_data(). Callers that use the echo-check
    // variant (e.g. FlashEcuMitsuM32rCanOperation) would otherwise fall
    // through to that real-hardware path and hang/warn on an unopened port.
    QByteArray write_serial_data_echo_check(QByteArray output) override
    {
        log("write_echo_check:begin:" + QString::fromLatin1(output.toHex()));
        if (readDelayMs)
            QThread::msleep(readDelayMs);
        log("write_echo_check:end");
        return QByteArray();
    }

private:
    void log(const QString &s)
    {
        QMutexLocker l(&logMutex);
        callLog.append(s);
    }
    QMutex logMutex;
    QStringList callLog;
};

#endif // FAKE_BACKEND_H
