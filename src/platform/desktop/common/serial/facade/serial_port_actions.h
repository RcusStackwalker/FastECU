#pragma once

#include <QCoreApplication>
#include <QMutex>
#include <QObject>
#include <QSemaphore>
#include <QThread>
#include <QtRemoteObjects/qremoteobjectnode.h>
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>

#include "src/platform/desktop/common/serial/serial_backend.h"
#include "src/platform/desktop/common/serial/serial_facade_codes.h"

class SerialBackendHost;

// Thin marshaling facade over a SerialBackend hosted on the SerialIoThread.
// The public surface is unchanged from the pre-refactor class; every method
// forwards to the backend on the I/O thread and blocks the caller until the
// result is ready.
class SerialPortActions : public QObject
{
    Q_OBJECT

  signals:
    // NOLINTBEGIN(readability-identifier-naming): Qt signals keep Qt's camelBack names
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State old_state);
    void logE(QString message, bool timestamp, bool linefeed);
    void logW(QString message, bool timestamp, bool linefeed);
    void logI(QString message, bool timestamp, bool linefeed);
    void logD(QString message, bool timestamp, bool linefeed);
    // NOLINTEND(readability-identifier-naming)

  public:
    // backend_factory builds the backend this facade drives; it is called
    // once, lazily, on the I/O thread. The composition root chooses it
    // (desktop_serial_factory.h); the facade never knows which it is.
    explicit SerialPortActions(std::function<SerialBackend *()> backend_factory, QObject *parent = nullptr);

    // Teardown-ordering precondition: callers must not start new calls while
    // the destructor is running. Calls already executing on the backend thread
    // are drained before ~SerialPortActions() joins the I/O thread.
    ~SerialPortActions();

    bool GetSerialPortAvailable();
    bool SetSerialPortAvailable(bool value);
    bool GetSetRequestToSend();
    bool SetSetRequestToSend(bool value);
    bool GetSetDataTerminalReady();
    bool SetSetDataTerminalReady(bool value);

    bool GetAddSsmHeader();
    bool SetAddSsmHeader(bool value);
    bool GetAddIso9141Header();
    bool SetAddIso9141Header(bool value);
    bool GetAddIso14230Header();
    bool SetAddIso14230Header(bool value);
    bool GetIsIso14230Connection();
    bool SetIsIso14230Connection(bool value);
    bool GetIsCanConnection();
    bool SetIsCanConnection(bool value);
    bool GetIsIso15765Connection();
    bool SetIsIso15765Connection(bool value);
    bool GetIs29BitId();
    bool SetIs29BitId(bool value);

    bool GetUseOpenport2Adapter();
    bool SetUseOpenport2Adapter(bool value);

    int GetRequestToSendEnabled();
    bool SetRequestToSendEnabled(int value);
    int GetRequestToSendDisabled();
    bool SetRequestToSendDisabled(int value);
    int GetDataTerminalEnabled();
    bool SetDataTerminalEnabled(int value);
    int GetDataTerminalDisabled();
    bool SetDataTerminalDisabled(int value);

    bool GetIsCommBusy();
    void SetCommBusy(bool value);
    bool GetReadVbatt();
    void SetReadVbatt(bool value);

    uint8_t GetKlineStartbyte();
    bool SetKlineStartbyte(uint8_t value);
    uint8_t GetKlineTesterId();
    bool SetKlineTesterId(uint8_t value);
    uint8_t GetKlineTargetId();
    bool SetKlineTargetId(uint8_t value);

    QByteArray GetSsmReceiveHeaderStart();
    bool SetSsmReceiveHeaderStart(const QByteArray& value);

    QStringList GetSerialPortList();
    bool SetSerialPortList(const QStringList& value);
    QString GetOpenedSerialPort();
    bool SetOpenedSerialPort(const QString& value);
    QString GetSubaru0216bitBootloaderBaudrate();
    bool SetSubaru0216bitBootloaderBaudrate(const QString& value);
    QString GetSubaru0416bitBootloaderBaudrate();
    bool SetSubaru0416bitBootloaderBaudrate(const QString& value);
    QString GetSubaru0232bitBootloaderBaudrate();
    bool SetSubaru0232bitBootloaderBaudrate(const QString& value);
    QString GetSubaru0432bitBootloaderBaudrate();
    bool SetSubaru0432bitBootloaderBaudrate(const QString& value);
    QString GetSubaru0532bitBootloaderBaudrate();
    bool SetSubaru0532bitBootloaderBaudrate(const QString& value);

    QString GetSubaru0216bitKernelBaudrate();
    bool SetSubaru0216bitKernelBaudrate(const QString& value);
    QString GetSubaru0416bitKernelBaudrate();
    bool SetSubaru0416bitKernelBaudrate(const QString& value);
    QString GetSubaru0232bitKernelBaudrate();
    bool SetSubaru0232bitKernelBaudrate(const QString& value);
    QString GetSubaru0432bitKernelBaudrate();
    bool SetSubaru0432bitKernelBaudrate(const QString& value);
    QString GetSubaru0532bitKernelBaudrate();
    bool SetSubaru0532bitKernelBaudrate(const QString& value);

    QString GetCanSpeed();
    bool SetCanSpeed(const QString& value);

    uint8_t GetSerialPortParity();
    bool SetSerialPortParity(uint8_t parity);
    QString GetSerialPortBaudrate();
    bool SetSerialPortBaudrate(const QString& value);
    QString GetSerialPortLinux();
    bool SetSerialPortLinux(const QString& value);
    QString GetSerialPortWindows();
    bool SetSerialPortWindows(const QString& value);
    QString GetSerialPort();
    bool SetSerialPort(const QString& value);
    QString GetSerialPortPrefix();
    bool SetSerialPortPrefix(const QString& value);
    QString GetSerialPortPrefixLinux();
    bool SetSerialPortPrefixLinux(const QString& value);
    QString GetSerialPortPrefixWin();
    bool SetSerialPortPrefixWin(const QString& value);

    uint32_t GetCanSourceAddress();
    bool SetCanSourceAddress(uint32_t value);
    uint32_t GetCanDestinationAddress();
    bool SetCanDestinationAddress(uint32_t value);
    uint32_t GetIso15765SourceAddress();
    bool SetIso15765SourceAddress(uint32_t value);
    uint32_t GetIso15765DestinationAddress();
    bool SetIso15765DestinationAddress(uint32_t value);

    bool SetKlineTimings(uint32_t parameter, int value);

    int SetJ2534Ioctl(uint32_t parameter, int value);

    bool IsSerialPortOpen(void);
    int ChangePortSpeed(const QString& port_speed);
    QByteArray FiveBaudInit(const QByteArray& output);
    int FastInit(const QByteArray& output);
    int SetLecLines(int lec1, int lec2);
    int PulseLec1Line(int timeout);
    int PulseLec2Line(int timeout);
    bool GetIsTxDone();

    bool ResetConnection(void);

    QByteArray ReadSerialObdData(uint16_t timeout);
    QByteArray ReadSerialData(uint16_t timeout);
    QByteArray WriteSerialData(const QByteArray& output);
    QByteArray WriteSerialDataEchoCheck(const QByteArray& output);

    int ClearRxBuffer(void);
    int ClearTxBuffer(void);

    int SendPeriodicJ2534Data(const QByteArray& output, int timeout);
    int StopPeriodicJ2534Data(void);

    QStringList CheckSerialPorts(void);
    QString OpenSerialPort(void);

    QString ParseMessageToHex(const QByteArray& received);

    unsigned long ReadVbatt();

  public slots:
    // NOLINTBEGIN(readability-identifier-naming): Qt slots keep Qt's camelBack names
    void waitForSource(void);
    // NOLINTEND(readability-identifier-naming)

  private:
    void EnsureBackendStarted();
    void WaitForDone(const std::shared_ptr<QSemaphore>& done);

    // RAII marker for a public call that still touches `this` after its
    // runOnBackend() call returns (e.g. read_serial_data()'s LOG_D emit and
    // set_comm_busy(false)). ~SerialPortActions() only drains work queued on
    // the I/O thread before joining it -- it has no visibility into a caller
    // thread that has already woken up from waitForDone() and is unwinding
    // through such trailing bookkeeping. Any public method with code after
    // its runOnBackend() call that reads or writes a member of `this` must
    // hold one of these for its whole body, or a concurrent destructor can
    // free the object out from under that caller.
    struct CallGuard
    {
        std::atomic<int>& counter;
        explicit CallGuard(std::atomic<int>& c) : counter(c)
        {
            counter.fetch_add(1);
        }
        ~CallGuard()
        {
            counter.fetch_sub(1);
        }

        // Non-copyable: each guard must decrement the counter exactly once,
        // matching the single increment its constructor performed.
        CallGuard(const CallGuard&) = delete;
        CallGuard& operator=(const CallGuard&) = delete;
    };

    // Marshal `fn` onto the I/O thread and block until it completes. `fn`
    // runs with m_backend valid and is the ONLY code that touches it.
    template <typename Fn> auto RunOnBackend(const Fn& fn)
    {
        using Ret = std::invoke_result_t<const Fn&>;
        EnsureBackendStarted();
        if (QThread::currentThread() == m_io_thread_)
        {
            return fn(); // already on the I/O thread (backend-side callback)
        }
        auto done = std::make_shared<QSemaphore>();
        std::exception_ptr failure;
        if constexpr (std::is_void_v<Ret>)
        {
            auto invoke = [fn, done, &failure]
            {
                struct Completion
                {
                    std::shared_ptr<QSemaphore> done;
                    ~Completion()
                    {
                        done->release();
                    }
                } completion{done};

                try
                {
                    fn();
                }
                catch (...)
                {
                    failure = std::current_exception();
                }
            };
            static_assert(std::is_invocable_v<const decltype(invoke)&>);
            QMetaObject::invokeMethod(m_io_context_, invoke, Qt::QueuedConnection);
            WaitForDone(done);
            if (failure)
            {
                std::rethrow_exception(failure);
            }
        }
        else
        {
            Ret result{};
            auto invoke = [fn, done, &result, &failure]
            {
                struct Completion
                {
                    std::shared_ptr<QSemaphore> done;
                    ~Completion()
                    {
                        done->release();
                    }
                } completion{done};

                try
                {
                    result = fn();
                }
                catch (...)
                {
                    failure = std::current_exception();
                }
            };
            static_assert(std::is_invocable_v<const decltype(invoke)&>);
            QMetaObject::invokeMethod(m_io_context_, invoke, Qt::QueuedConnection);
            WaitForDone(done);
            if (failure)
            {
                std::rethrow_exception(failure);
            }
            return result;
        }
    }

    std::function<SerialBackend *()> backend_factory_;

    QMutex start_mutex_;
    SerialBackendHost *m_host_ = nullptr;
    SerialBackend *m_backend_ = nullptr;
    QObject *m_io_context_ = nullptr; // == m_host->context(), cached
    QThread *m_io_thread_ = nullptr;  // == m_host->ioThread(), cached

    QAtomicInteger<bool> is_read_vbatt_ = false;
    QAtomicInteger<bool> is_comm_busy_ = false;
    std::atomic<unsigned long> v_batt_{0};
    std::atomic<int> m_active_calls_{0};
};
