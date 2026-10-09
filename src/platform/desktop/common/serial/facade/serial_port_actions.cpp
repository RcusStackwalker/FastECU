#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

#include <utility>

#include "src/platform/desktop/common/serial/serial_backend_host.h"

SerialPortActions::SerialPortActions(std::function<SerialBackend *()> backend_factory, QObject *parent)
    : QObject{parent}, backend_factory_(std::move(backend_factory))
{
}

SerialPortActions::~SerialPortActions()
{
    QMutexLocker locker(&start_mutex_);
    // Precondition (see serial_port_actions.h): no new caller may enter this
    // facade while teardown is running. Work already queued on the backend
    // thread is drained before `delete m_host` joins the thread.
    delete m_host_; // deletes the backend on the I/O thread, joins the thread
    m_host_ = nullptr;
    m_backend_ = nullptr;
    // A caller whose backend call just finished (e.g. read_serial_data())
    // may still be unwinding through CallGuard-protected bookkeeping on its
    // own thread at this point -- draining the I/O thread above only proves
    // the backend-side half of that call is done. Wait for it to fully
    // return before this object's memory is freed.
    while (m_active_calls_.load() > 0)
    {
        QThread::yieldCurrentThread();
    }
}

void SerialPortActions::EnsureBackendStarted()
{
    QMutexLocker locker(&start_mutex_);
    if (m_backend_)
    {
        return;
    }
    m_host_ = new SerialBackendHost();
    m_io_context_ = m_host_->Context();
    m_io_thread_ = m_host_->IoThread();
    m_backend_ = m_host_->CreateBackend(backend_factory_);

    QObject *b = m_backend_->Qobject();
    connect(b, SIGNAL(LOG_E(QString, bool, bool)), this, SIGNAL(LOG_E(QString, bool, bool)));
    connect(b, SIGNAL(LOG_W(QString, bool, bool)), this, SIGNAL(LOG_W(QString, bool, bool)));
    connect(b, SIGNAL(LOG_I(QString, bool, bool)), this, SIGNAL(LOG_I(QString, bool, bool)));
    connect(b, SIGNAL(LOG_D(QString, bool, bool)), this, SIGNAL(LOG_D(QString, bool, bool)));
    if (b->metaObject()->indexOfSignal(QMetaObject::normalizedSignature(
            "stateChanged(QRemoteObjectReplica::State,QRemoteObjectReplica::State)")) >= 0)
    {
        connect(b, SIGNAL(stateChanged(QRemoteObjectReplica::State, QRemoteObjectReplica::State)), this,
                SIGNAL(stateChanged(QRemoteObjectReplica::State, QRemoteObjectReplica::State)));
    }
}

void SerialPortActions::WaitForDone(const std::shared_ptr<QSemaphore>& done)
{
    // No GUI-thread pump: every caller either runs on its own worker thread
    // (flash-module operations, LoggingWorker) or accepts a brief blocking
    // wait for a short, click-bounded call (BIU, DataTerminal, MainWindow's
    // connection code). DTC now runs its session on DtcWorker, off the UI
    // thread, and the legacy hexcommander dialog has been removed.
    done->acquire();
}

void SerialPortActions::waitForSource(void)
{
    RunOnBackend([this] { m_backend_->WaitForSource(); });
}

// -- config get/set pairs (44) ------------------------------------------

bool SerialPortActions::GetSerialPortAvailable(void)
{
    return RunOnBackend([this] { return m_backend_->GetSerialPortAvailable(); });
}
bool SerialPortActions::SetSerialPortAvailable(bool value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSerialPortAvailable(value); });
}
bool SerialPortActions::GetSetRequestToSend(void)
{
    return RunOnBackend([this] { return m_backend_->GetSetRequestToSend(); });
}
bool SerialPortActions::SetSetRequestToSend(bool value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSetRequestToSend(value); });
}
bool SerialPortActions::GetSetDataTerminalReady(void)
{
    return RunOnBackend([this] { return m_backend_->GetSetDataTerminalReady(); });
}
bool SerialPortActions::SetSetDataTerminalReady(bool value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSetDataTerminalReady(value); });
}

bool SerialPortActions::GetAddSsmHeader(void)
{
    return RunOnBackend([this] { return m_backend_->GetAddSsmHeader(); });
}
bool SerialPortActions::SetAddSsmHeader(bool value)
{
    return RunOnBackend([this, value] { return m_backend_->SetAddSsmHeader(value); });
}
bool SerialPortActions::GetAddIso9141Header(void)
{
    return RunOnBackend([this] { return m_backend_->GetAddIso9141Header(); });
}
bool SerialPortActions::SetAddIso9141Header(bool value)
{
    return RunOnBackend([this, value] { return m_backend_->SetAddIso9141Header(value); });
}
bool SerialPortActions::GetAddIso14230Header(void)
{
    return RunOnBackend([this] { return m_backend_->GetAddIso14230Header(); });
}
bool SerialPortActions::SetAddIso14230Header(bool value)
{
    return RunOnBackend([this, value] { return m_backend_->SetAddIso14230Header(value); });
}
bool SerialPortActions::GetIsIso14230Connection(void)
{
    return RunOnBackend([this] { return m_backend_->GetIsIso14230Connection(); });
}
bool SerialPortActions::SetIsIso14230Connection(bool value)
{
    return RunOnBackend([this, value] { return m_backend_->SetIsIso14230Connection(value); });
}
bool SerialPortActions::GetIsCanConnection(void)
{
    return RunOnBackend([this] { return m_backend_->GetIsCanConnection(); });
}
bool SerialPortActions::SetIsCanConnection(bool value)
{
    return RunOnBackend([this, value] { return m_backend_->SetIsCanConnection(value); });
}
bool SerialPortActions::GetIsIso15765Connection(void)
{
    return RunOnBackend([this] { return m_backend_->GetIsIso15765Connection(); });
}
bool SerialPortActions::SetIsIso15765Connection(bool value)
{
    return RunOnBackend([this, value] { return m_backend_->SetIsIso15765Connection(value); });
}
bool SerialPortActions::GetIs29BitId(void)
{
    return RunOnBackend([this] { return m_backend_->GetIs29BitId(); });
}
bool SerialPortActions::SetIs29BitId(bool value)
{
    return RunOnBackend([this, value] { return m_backend_->SetIs29BitId(value); });
}

bool SerialPortActions::GetUseOpenport2Adapter(void)
{
    return RunOnBackend([this] { return m_backend_->GetUseOpenport2Adapter(); });
}
bool SerialPortActions::SetUseOpenport2Adapter(bool value)
{
    return RunOnBackend([this, value] { return m_backend_->SetUseOpenport2Adapter(value); });
}

int SerialPortActions::GetRequestToSendEnabled(void)
{
    return RunOnBackend([this] { return m_backend_->GetRequestToSendEnabled(); });
}
bool SerialPortActions::SetRequestToSendEnabled(int value)
{
    return RunOnBackend([this, value] { return m_backend_->SetRequestToSendEnabled(value); });
}
int SerialPortActions::GetRequestToSendDisabled(void)
{
    return RunOnBackend([this] { return m_backend_->GetRequestToSendDisabled(); });
}
bool SerialPortActions::SetRequestToSendDisabled(int value)
{
    return RunOnBackend([this, value] { return m_backend_->SetRequestToSendDisabled(value); });
}
int SerialPortActions::GetDataTerminalEnabled(void)
{
    return RunOnBackend([this] { return m_backend_->GetDataTerminalEnabled(); });
}
bool SerialPortActions::SetDataTerminalEnabled(int value)
{
    return RunOnBackend([this, value] { return m_backend_->SetDataTerminalEnabled(value); });
}
int SerialPortActions::GetDataTerminalDisabled(void)
{
    return RunOnBackend([this] { return m_backend_->GetDataTerminalDisabled(); });
}
bool SerialPortActions::SetDataTerminalDisabled(int value)
{
    return RunOnBackend([this, value] { return m_backend_->SetDataTerminalDisabled(value); });
}

uint8_t SerialPortActions::GetKlineStartbyte(void)
{
    return RunOnBackend([this] { return m_backend_->GetKlineStartbyte(); });
}
bool SerialPortActions::SetKlineStartbyte(uint8_t value)
{
    return RunOnBackend([this, value] { return m_backend_->SetKlineStartbyte(value); });
}
uint8_t SerialPortActions::GetKlineTesterId(void)
{
    return RunOnBackend([this] { return m_backend_->GetKlineTesterId(); });
}
bool SerialPortActions::SetKlineTesterId(uint8_t value)
{
    return RunOnBackend([this, value] { return m_backend_->SetKlineTesterId(value); });
}
uint8_t SerialPortActions::GetKlineTargetId(void)
{
    return RunOnBackend([this] { return m_backend_->GetKlineTargetId(); });
}
bool SerialPortActions::SetKlineTargetId(uint8_t value)
{
    return RunOnBackend([this, value] { return m_backend_->SetKlineTargetId(value); });
}

QByteArray SerialPortActions::GetSsmReceiveHeaderStart(void)
{
    return RunOnBackend([this] { return m_backend_->GetSsmReceiveHeaderStart(); });
}
bool SerialPortActions::SetSsmReceiveHeaderStart(const QByteArray& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSsmReceiveHeaderStart(value); });
}

QStringList SerialPortActions::GetSerialPortList(void)
{
    return RunOnBackend([this] { return m_backend_->GetSerialPortList(); });
}
bool SerialPortActions::SetSerialPortList(const QStringList& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSerialPortList(value); });
}
QString SerialPortActions::GetOpenedSerialPort(void)
{
    return RunOnBackend([this] { return m_backend_->GetOpenedSerialPort(); });
}
bool SerialPortActions::SetOpenedSerialPort(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetOpenedSerialPort(value); });
}
QString SerialPortActions::GetSubaru0216bitBootloaderBaudrate(void)
{
    return RunOnBackend([this] { return m_backend_->GetSubaru0216bitBootloaderBaudrate(); });
}
bool SerialPortActions::SetSubaru0216bitBootloaderBaudrate(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSubaru0216bitBootloaderBaudrate(value); });
}
QString SerialPortActions::GetSubaru0416bitBootloaderBaudrate(void)
{
    return RunOnBackend([this] { return m_backend_->GetSubaru0416bitBootloaderBaudrate(); });
}
bool SerialPortActions::SetSubaru0416bitBootloaderBaudrate(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSubaru0416bitBootloaderBaudrate(value); });
}
QString SerialPortActions::GetSubaru0232bitBootloaderBaudrate(void)
{
    return RunOnBackend([this] { return m_backend_->GetSubaru0232bitBootloaderBaudrate(); });
}
bool SerialPortActions::SetSubaru0232bitBootloaderBaudrate(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSubaru0232bitBootloaderBaudrate(value); });
}
QString SerialPortActions::GetSubaru0432bitBootloaderBaudrate(void)
{
    return RunOnBackend([this] { return m_backend_->GetSubaru0432bitBootloaderBaudrate(); });
}
bool SerialPortActions::SetSubaru0432bitBootloaderBaudrate(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSubaru0432bitBootloaderBaudrate(value); });
}
QString SerialPortActions::GetSubaru0532bitBootloaderBaudrate(void)
{
    return RunOnBackend([this] { return m_backend_->GetSubaru0532bitBootloaderBaudrate(); });
}
bool SerialPortActions::SetSubaru0532bitBootloaderBaudrate(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSubaru0532bitBootloaderBaudrate(value); });
}

QString SerialPortActions::GetSubaru0216bitKernelBaudrate(void)
{
    return RunOnBackend([this] { return m_backend_->GetSubaru0216bitKernelBaudrate(); });
}
bool SerialPortActions::SetSubaru0216bitKernelBaudrate(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSubaru0216bitKernelBaudrate(value); });
}
QString SerialPortActions::GetSubaru0416bitKernelBaudrate(void)
{
    return RunOnBackend([this] { return m_backend_->GetSubaru0416bitKernelBaudrate(); });
}
bool SerialPortActions::SetSubaru0416bitKernelBaudrate(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSubaru0416bitKernelBaudrate(value); });
}
QString SerialPortActions::GetSubaru0232bitKernelBaudrate(void)
{
    return RunOnBackend([this] { return m_backend_->GetSubaru0232bitKernelBaudrate(); });
}
bool SerialPortActions::SetSubaru0232bitKernelBaudrate(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSubaru0232bitKernelBaudrate(value); });
}
QString SerialPortActions::GetSubaru0432bitKernelBaudrate(void)
{
    return RunOnBackend([this] { return m_backend_->GetSubaru0432bitKernelBaudrate(); });
}
bool SerialPortActions::SetSubaru0432bitKernelBaudrate(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSubaru0432bitKernelBaudrate(value); });
}
QString SerialPortActions::GetSubaru0532bitKernelBaudrate(void)
{
    return RunOnBackend([this] { return m_backend_->GetSubaru0532bitKernelBaudrate(); });
}
bool SerialPortActions::SetSubaru0532bitKernelBaudrate(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSubaru0532bitKernelBaudrate(value); });
}

QString SerialPortActions::GetCanSpeed(void)
{
    return RunOnBackend([this] { return m_backend_->GetCanSpeed(); });
}
bool SerialPortActions::SetCanSpeed(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetCanSpeed(value); });
}
uint8_t SerialPortActions::GetSerialPortParity(void)
{
    return RunOnBackend([this] { return m_backend_->GetSerialPortParity(); });
}
bool SerialPortActions::SetSerialPortParity(uint8_t parity)
{
    return RunOnBackend([this, parity] { return m_backend_->SetSerialPortParity(parity); });
}
QString SerialPortActions::GetSerialPortBaudrate(void)
{
    return RunOnBackend([this] { return m_backend_->GetSerialPortBaudrate(); });
}
bool SerialPortActions::SetSerialPortBaudrate(const QString& value)
{
    emit LOG_D("Setting serialport baudrate in SerialPortActions", true, true);
    return RunOnBackend([this, value] { return m_backend_->SetSerialPortBaudrate(value); });
}
QString SerialPortActions::GetSerialPortLinux(void)
{
    return RunOnBackend([this] { return m_backend_->GetSerialPortLinux(); });
}
bool SerialPortActions::SetSerialPortLinux(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSerialPortLinux(value); });
}
QString SerialPortActions::GetSerialPortWindows(void)
{
    return RunOnBackend([this] { return m_backend_->GetSerialPortWindows(); });
}
bool SerialPortActions::SetSerialPortWindows(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSerialPortWindows(value); });
}
QString SerialPortActions::GetSerialPort(void)
{
    return RunOnBackend([this] { return m_backend_->GetSerialPort(); });
}
bool SerialPortActions::SetSerialPort(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSerialPort(value); });
}
QString SerialPortActions::GetSerialPortPrefix(void)
{
    return RunOnBackend([this] { return m_backend_->GetSerialPortPrefix(); });
}
bool SerialPortActions::SetSerialPortPrefix(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSerialPortPrefix(value); });
}
QString SerialPortActions::GetSerialPortPrefixLinux(void)
{
    return RunOnBackend([this] { return m_backend_->GetSerialPortPrefixLinux(); });
}
bool SerialPortActions::SetSerialPortPrefixLinux(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSerialPortPrefixLinux(value); });
}
QString SerialPortActions::GetSerialPortPrefixWin(void)
{
    return RunOnBackend([this] { return m_backend_->GetSerialPortPrefixWin(); });
}
bool SerialPortActions::SetSerialPortPrefixWin(const QString& value)
{
    return RunOnBackend([this, value] { return m_backend_->SetSerialPortPrefixWin(value); });
}

uint32_t SerialPortActions::GetCanSourceAddress(void)
{
    return RunOnBackend([this] { return m_backend_->GetCanSourceAddress(); });
}
bool SerialPortActions::SetCanSourceAddress(uint32_t value)
{
    return RunOnBackend([this, value] { return m_backend_->SetCanSourceAddress(value); });
}
uint32_t SerialPortActions::GetCanDestinationAddress(void)
{
    return RunOnBackend([this] { return m_backend_->GetCanDestinationAddress(); });
}
bool SerialPortActions::SetCanDestinationAddress(uint32_t value)
{
    return RunOnBackend([this, value] { return m_backend_->SetCanDestinationAddress(value); });
}
uint32_t SerialPortActions::GetIso15765SourceAddress(void)
{
    return RunOnBackend([this] { return m_backend_->GetIso15765SourceAddress(); });
}
bool SerialPortActions::SetIso15765SourceAddress(uint32_t value)
{
    return RunOnBackend([this, value] { return m_backend_->SetIso15765SourceAddress(value); });
}
uint32_t SerialPortActions::GetIso15765DestinationAddress(void)
{
    return RunOnBackend([this] { return m_backend_->GetIso15765DestinationAddress(); });
}
bool SerialPortActions::SetIso15765DestinationAddress(uint32_t value)
{
    return RunOnBackend([this, value] { return m_backend_->SetIso15765DestinationAddress(value); });
}

// -- operations ---------------------------------------------------------

bool SerialPortActions::IsSerialPortOpen()
{
    return RunOnBackend([this] { return m_backend_->IsSerialPortOpen(); });
}

int SerialPortActions::ChangePortSpeed(const QString& port_speed)
{
    CallGuard guard(m_active_calls_);
    SetCommBusy(true);
    int result = RunOnBackend([this, port_speed] { return m_backend_->ChangePortSpeed(port_speed); });
    SetCommBusy(false);
    return result;
}

bool SerialPortActions::SetKlineTimings(uint32_t parameter, int value)
{
    return RunOnBackend([this, parameter, value] { return m_backend_->SetKlineTimings(parameter, value); });
}

int SerialPortActions::SetJ2534Ioctl(uint32_t parameter, int value)
{
    SetCommBusy(true);
    return RunOnBackend([this, parameter, value] { return m_backend_->SetJ2534Ioctl(parameter, value); });
}

QByteArray SerialPortActions::FiveBaudInit(const QByteArray& output)
{
    SetCommBusy(true);
    return RunOnBackend([this, output] { return m_backend_->FiveBaudInit(output); });
}

int SerialPortActions::FastInit(const QByteArray& output)
{
    SetCommBusy(true);
    return RunOnBackend([this, output] { return m_backend_->FastInit(output); });
}

int SerialPortActions::SetLecLines(int lec1, int lec2)
{
    CallGuard guard(m_active_calls_);
    SetCommBusy(true);
    int result = RunOnBackend([this, lec1, lec2] { return m_backend_->SetLecLines(lec1, lec2); });
    SetCommBusy(false);
    return result;
}

int SerialPortActions::PulseLec1Line(int timeout)
{
    CallGuard guard(m_active_calls_);
    SetCommBusy(true);
    int result = RunOnBackend([this, timeout] { return m_backend_->PulseLec1Line(timeout); });
    SetCommBusy(false);
    return result;
}

int SerialPortActions::PulseLec2Line(int timeout)
{
    CallGuard guard(m_active_calls_);
    SetCommBusy(true);
    int result = RunOnBackend([this, timeout] { return m_backend_->PulseLec2Line(timeout); });
    SetCommBusy(false);
    return result;
}

bool SerialPortActions::GetIsCommBusy()
{
    return is_comm_busy_;
}

void SerialPortActions::SetCommBusy(bool value)
{
    is_comm_busy_ = value;
}

bool SerialPortActions::GetReadVbatt()
{
    return is_read_vbatt_;
}

void SerialPortActions::SetReadVbatt(bool value)
{
    is_read_vbatt_ = value;
}

bool SerialPortActions::ResetConnection()
{
    RunOnBackend([this] { m_backend_->ResetConnection(); });
    return true;
}

QByteArray SerialPortActions::ReadSerialObdData(uint16_t timeout)
{
    CallGuard guard(m_active_calls_);
    QByteArray response = RunOnBackend([this, timeout] { return m_backend_->ReadSerialObdData(timeout); });
    emit LOG_D("Response: " + ParseMessageToHex(response.mid(0, 20)), true, true);
    SetCommBusy(false);
    return response;
}

QByteArray SerialPortActions::ReadSerialData(uint16_t timeout)
{
    CallGuard guard(m_active_calls_);
    QByteArray response = RunOnBackend(
        [this, timeout]
        {
            QByteArray r = m_backend_->ReadSerialData(timeout);
            if (GetReadVbatt())
            {
                v_batt_.store(m_backend_->ReadVbatt());
                SetReadVbatt(false);
            }
            return r;
        });
    emit LOG_D("Response: " + ParseMessageToHex(response.mid(0, 20)), true, true);
    SetCommBusy(false);
    return response;
}

QByteArray SerialPortActions::WriteSerialData(const QByteArray& output)
{
    SetCommBusy(true);
    emit LOG_D("Sent: " + ParseMessageToHex(output.mid(0, 20)), true, true);
    return RunOnBackend([this, output] { return m_backend_->WriteSerialData(output); });
}

QByteArray SerialPortActions::WriteSerialDataEchoCheck(const QByteArray& output)
{
    SetCommBusy(true);
    emit LOG_D("Sent: " + ParseMessageToHex(output.mid(0, 20)), true, true);
    return RunOnBackend([this, output] { return m_backend_->WriteSerialDataEchoCheck(output); });
}

bool SerialPortActions::GetIsTxDone()
{
    return RunOnBackend([this] { return m_backend_->GetIsTxDone(); });
}

int SerialPortActions::ClearRxBuffer()
{
    CallGuard guard(m_active_calls_);
    SetCommBusy(true);
    int result = RunOnBackend([this] { return m_backend_->ClearRxBuffer(); });
    SetCommBusy(false);
    return result;
}

int SerialPortActions::ClearTxBuffer()
{
    CallGuard guard(m_active_calls_);
    SetCommBusy(true);
    int result = RunOnBackend([this] { return m_backend_->ClearTxBuffer(); });
    SetCommBusy(false);
    return result;
}

int SerialPortActions::SendPeriodicJ2534Data(const QByteArray& output, int timeout)
{
    CallGuard guard(m_active_calls_);
    SetCommBusy(true);
    int result = RunOnBackend([this, output, timeout] { return m_backend_->SendPeriodicJ2534Data(output, timeout); });
    SetCommBusy(false);
    return result;
}

int SerialPortActions::StopPeriodicJ2534Data()
{
    CallGuard guard(m_active_calls_);
    SetCommBusy(true);
    int result = RunOnBackend([this] { return m_backend_->StopPeriodicJ2534Data(); });
    SetCommBusy(false);
    return result;
}

QStringList SerialPortActions::CheckSerialPorts()
{
    return RunOnBackend([this] { return m_backend_->CheckSerialPorts(); });
}

QString SerialPortActions::OpenSerialPort()
{
    return RunOnBackend([this] { return m_backend_->OpenSerialPort(); });
}

unsigned long SerialPortActions::ReadVbatt()
{
    CallGuard guard(m_active_calls_);
    if (!GetIsCommBusy() && !GetReadVbatt())
    {
        v_batt_.store(RunOnBackend([this] { return m_backend_->ReadVbatt(); }));
    }
    else
    {
        SetReadVbatt(true);
    }
    return v_batt_.load();
}

QString SerialPortActions::ParseMessageToHex(const QByteArray& received)
{
    QString msg;

    for (int i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1 ").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUtf8());
    }

    return msg;
}
