#ifndef SERIAL_BACKEND_HOST_H
#define SERIAL_BACKEND_HOST_H

#include <QObject>
#include <QThread>
#include <functional>

class SerialBackend;

// Owns the SerialIoThread and the backend living on it. The spec's
// "SerialIoThread" component is this class's m_thread. All backend calls are
// marshaled by SerialPortActions onto context(); the backend is constructed
// and destructed on the thread, so QSerialPort/QSocketNotifier affinity is
// correct by construction.
class SerialBackendHost
{
public:
    SerialBackendHost();
    ~SerialBackendHost();   // deletes the backend on the I/O thread, then joins

    // Runs `factory` on the I/O thread (blocking) and returns the backend.
    SerialBackend *createBackend(const std::function<SerialBackend *()> &factory);

    QObject *context() const { return m_context; }
    QThread *ioThread() { return &m_thread; }

private:
    QThread m_thread;
    QObject *m_context = nullptr;   // affinity: m_thread; invokeMethod target
    SerialBackend *m_backend = nullptr;
};

#endif // SERIAL_BACKEND_HOST_H
