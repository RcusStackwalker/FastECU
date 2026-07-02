#ifndef FAKE_BACKEND_H
#define FAKE_BACKEND_H

#include <QMutex>
#include <QMutexLocker>
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

    QStringList takeCallLog()
    {
        QMutexLocker l(&logMutex);
        QStringList out = callLog;
        callLog.clear();
        return out;
    }

    bool is_serial_port_open() override { return true; }

    QByteArray read_serial_data(uint16_t timeout) override
    {
        log(QString("read:begin:t=%1").arg(timeout));
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
