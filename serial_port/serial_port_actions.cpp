#include "serial_port_actions.h"

#include "remote_serial_backend.h"
#include "serial_backend_host.h"

SerialPortActions::SerialPortActions(QString peerAddress, QString password,
                                     QWebSocket *web_socket, QObject *parent,
                                     std::function<SerialBackend *()> backendFactoryForTests)
    : QObject{parent}
    , peerAddress(peerAddress)
    , password(password)
    , externalSocket(web_socket)
    , backendFactory(std::move(backendFactoryForTests))
{
    if (!backendFactory)
    {
        backendFactory = [this]() -> SerialBackend * {
            if (isDirectConnection())
                return new SerialPortActionsDirect();
            return new RemoteSerialBackend(this->peerAddress, this->password,
                                           this->externalSocket);
        };
    }
}

SerialPortActions::~SerialPortActions()
{
    QMutexLocker locker(&startMutex);
    // Precondition (see the doc comment on this destructor in
    // serial_port_actions.h): no other thread may be blocked in a call to
    // this object right now. `delete m_host` joins the I/O thread and drops
    // any lambda queued via runOnBackend() that hasn't run yet, so a caller
    // still waiting on that lambda's semaphore hangs here rather than
    // crashing on a freed object as it did pre-refactor.
    delete m_host;   // deletes the backend on the I/O thread, joins the thread
    m_host = nullptr;
    m_backend = nullptr;
}

bool SerialPortActions::isDirectConnection(void)
{
    return (peerAddress == "");
}

void SerialPortActions::ensureBackendStarted()
{
    QMutexLocker locker(&startMutex);
    if (m_backend)
        return;
    m_host = new SerialBackendHost();
    m_ioContext = m_host->context();
    m_ioThread = m_host->ioThread();
    m_backend = m_host->createBackend(backendFactory);

    QObject *b = m_backend->qobject();
    connect(b, SIGNAL(LOG_E(QString,bool,bool)), this, SIGNAL(LOG_E(QString,bool,bool)));
    connect(b, SIGNAL(LOG_W(QString,bool,bool)), this, SIGNAL(LOG_W(QString,bool,bool)));
    connect(b, SIGNAL(LOG_I(QString,bool,bool)), this, SIGNAL(LOG_I(QString,bool,bool)));
    connect(b, SIGNAL(LOG_D(QString,bool,bool)), this, SIGNAL(LOG_D(QString,bool,bool)));
    if (b->metaObject()->indexOfSignal(
            QMetaObject::normalizedSignature(
                "stateChanged(QRemoteObjectReplica::State,QRemoteObjectReplica::State)")) >= 0)
        connect(b, SIGNAL(stateChanged(QRemoteObjectReplica::State,QRemoteObjectReplica::State)),
                this, SIGNAL(stateChanged(QRemoteObjectReplica::State,QRemoteObjectReplica::State)));
}

void SerialPortActions::waitForDone(const std::shared_ptr<QSemaphore> &done)
{
    // No GUI-thread pump: every caller either runs on its own worker thread
    // (flash-module operations, LoggingWorker) or accepts a brief blocking
    // wait for a short, click-bounded call (dtc_operations, dataterminal,
    // hexcommander).
    done->acquire();
}

void SerialPortActions::waitForSource(void)
{
    runOnBackend([this] { m_backend->waitForSource(); });
}

// -- config get/set pairs (44) ------------------------------------------

bool SerialPortActions::get_serialPortAvailable(void)
{
    return runOnBackend([this] { return m_backend->get_serialPortAvailable(); });
}
bool SerialPortActions::set_serialPortAvailable(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_serialPortAvailable(value); });
}
bool SerialPortActions::get_setRequestToSend(void)
{
    return runOnBackend([this] { return m_backend->get_setRequestToSend(); });
}
bool SerialPortActions::set_setRequestToSend(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_setRequestToSend(value); });
}
bool SerialPortActions::get_setDataTerminalReady(void)
{
    return runOnBackend([this] { return m_backend->get_setDataTerminalReady(); });
}
bool SerialPortActions::set_setDataTerminalReady(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_setDataTerminalReady(value); });
}

bool SerialPortActions::get_add_ssm_header(void)
{
    return runOnBackend([this] { return m_backend->get_add_ssm_header(); });
}
bool SerialPortActions::set_add_ssm_header(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_add_ssm_header(value); });
}
bool SerialPortActions::get_add_iso9141_header(void)
{
    return runOnBackend([this] { return m_backend->get_add_iso9141_header(); });
}
bool SerialPortActions::set_add_iso9141_header(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_add_iso9141_header(value); });
}
bool SerialPortActions::get_add_iso14230_header(void)
{
    return runOnBackend([this] { return m_backend->get_add_iso14230_header(); });
}
bool SerialPortActions::set_add_iso14230_header(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_add_iso14230_header(value); });
}
bool SerialPortActions::get_is_iso14230_connection(void)
{
    return runOnBackend([this] { return m_backend->get_is_iso14230_connection(); });
}
bool SerialPortActions::set_is_iso14230_connection(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_is_iso14230_connection(value); });
}
bool SerialPortActions::get_is_can_connection(void)
{
    return runOnBackend([this] { return m_backend->get_is_can_connection(); });
}
bool SerialPortActions::set_is_can_connection(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_is_can_connection(value); });
}
bool SerialPortActions::get_is_iso15765_connection(void)
{
    return runOnBackend([this] { return m_backend->get_is_iso15765_connection(); });
}
bool SerialPortActions::set_is_iso15765_connection(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_is_iso15765_connection(value); });
}
bool SerialPortActions::get_is_29_bit_id(void)
{
    return runOnBackend([this] { return m_backend->get_is_29_bit_id(); });
}
bool SerialPortActions::set_is_29_bit_id(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_is_29_bit_id(value); });
}

bool SerialPortActions::get_use_openport2_adapter(void)
{
    return runOnBackend([this] { return m_backend->get_use_openport2_adapter(); });
}
bool SerialPortActions::set_use_openport2_adapter(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_use_openport2_adapter(value); });
}

int SerialPortActions::get_requestToSendEnabled(void)
{
    return runOnBackend([this] { return m_backend->get_requestToSendEnabled(); });
}
bool SerialPortActions::set_requestToSendEnabled(int value)
{
    return runOnBackend([this, value] { return m_backend->set_requestToSendEnabled(value); });
}
int SerialPortActions::get_requestToSendDisabled(void)
{
    return runOnBackend([this] { return m_backend->get_requestToSendDisabled(); });
}
bool SerialPortActions::set_requestToSendDisabled(int value)
{
    return runOnBackend([this, value] { return m_backend->set_requestToSendDisabled(value); });
}
int SerialPortActions::get_dataTerminalEnabled(void)
{
    return runOnBackend([this] { return m_backend->get_dataTerminalEnabled(); });
}
bool SerialPortActions::set_dataTerminalEnabled(int value)
{
    return runOnBackend([this, value] { return m_backend->set_dataTerminalEnabled(value); });
}
int SerialPortActions::get_dataTerminalDisabled(void)
{
    return runOnBackend([this] { return m_backend->get_dataTerminalDisabled(); });
}
bool SerialPortActions::set_dataTerminalDisabled(int value)
{
    return runOnBackend([this, value] { return m_backend->set_dataTerminalDisabled(value); });
}

uint8_t SerialPortActions::get_kline_startbyte(void)
{
    return runOnBackend([this] { return m_backend->get_kline_startbyte(); });
}
bool SerialPortActions::set_kline_startbyte(uint8_t value)
{
    return runOnBackend([this, value] { return m_backend->set_kline_startbyte(value); });
}
uint8_t SerialPortActions::get_kline_tester_id(void)
{
    return runOnBackend([this] { return m_backend->get_kline_tester_id(); });
}
bool SerialPortActions::set_kline_tester_id(uint8_t value)
{
    return runOnBackend([this, value] { return m_backend->set_kline_tester_id(value); });
}
uint8_t SerialPortActions::get_kline_target_id(void)
{
    return runOnBackend([this] { return m_backend->get_kline_target_id(); });
}
bool SerialPortActions::set_kline_target_id(uint8_t value)
{
    return runOnBackend([this, value] { return m_backend->set_kline_target_id(value); });
}

QByteArray SerialPortActions::get_ssm_receive_header_start(void)
{
    return runOnBackend([this] { return m_backend->get_ssm_receive_header_start(); });
}
bool SerialPortActions::set_ssm_receive_header_start(QByteArray value)
{
    return runOnBackend([this, value] { return m_backend->set_ssm_receive_header_start(value); });
}

QStringList SerialPortActions::get_serial_port_list(void)
{
    return runOnBackend([this] { return m_backend->get_serial_port_list(); });
}
bool SerialPortActions::set_serial_port_list(QStringList value)
{
    return runOnBackend([this, value] { return m_backend->set_serial_port_list(value); });
}
QString SerialPortActions::get_openedSerialPort(void)
{
    return runOnBackend([this] { return m_backend->get_openedSerialPort(); });
}
bool SerialPortActions::set_openedSerialPort(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_openedSerialPort(value); });
}
QString SerialPortActions::get_subaru_02_16bit_bootloader_baudrate(void)
{
    return runOnBackend([this] { return m_backend->get_subaru_02_16bit_bootloader_baudrate(); });
}
bool SerialPortActions::set_subaru_02_16bit_bootloader_baudrate(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_subaru_02_16bit_bootloader_baudrate(value); });
}
QString SerialPortActions::get_subaru_04_16bit_bootloader_baudrate(void)
{
    return runOnBackend([this] { return m_backend->get_subaru_04_16bit_bootloader_baudrate(); });
}
bool SerialPortActions::set_subaru_04_16bit_bootloader_baudrate(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_subaru_04_16bit_bootloader_baudrate(value); });
}
QString SerialPortActions::get_subaru_02_32bit_bootloader_baudrate(void)
{
    return runOnBackend([this] { return m_backend->get_subaru_02_32bit_bootloader_baudrate(); });
}
bool SerialPortActions::set_subaru_02_32bit_bootloader_baudrate(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_subaru_02_32bit_bootloader_baudrate(value); });
}
QString SerialPortActions::get_subaru_04_32bit_bootloader_baudrate(void)
{
    return runOnBackend([this] { return m_backend->get_subaru_04_32bit_bootloader_baudrate(); });
}
bool SerialPortActions::set_subaru_04_32bit_bootloader_baudrate(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_subaru_04_32bit_bootloader_baudrate(value); });
}
QString SerialPortActions::get_subaru_05_32bit_bootloader_baudrate(void)
{
    return runOnBackend([this] { return m_backend->get_subaru_05_32bit_bootloader_baudrate(); });
}
bool SerialPortActions::set_subaru_05_32bit_bootloader_baudrate(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_subaru_05_32bit_bootloader_baudrate(value); });
}

QString SerialPortActions::get_subaru_02_16bit_kernel_baudrate(void)
{
    return runOnBackend([this] { return m_backend->get_subaru_02_16bit_kernel_baudrate(); });
}
bool SerialPortActions::set_subaru_02_16bit_kernel_baudrate(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_subaru_02_16bit_kernel_baudrate(value); });
}
QString SerialPortActions::get_subaru_04_16bit_kernel_baudrate(void)
{
    return runOnBackend([this] { return m_backend->get_subaru_04_16bit_kernel_baudrate(); });
}
bool SerialPortActions::set_subaru_04_16bit_kernel_baudrate(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_subaru_04_16bit_kernel_baudrate(value); });
}
QString SerialPortActions::get_subaru_02_32bit_kernel_baudrate(void)
{
    return runOnBackend([this] { return m_backend->get_subaru_02_32bit_kernel_baudrate(); });
}
bool SerialPortActions::set_subaru_02_32bit_kernel_baudrate(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_subaru_02_32bit_kernel_baudrate(value); });
}
QString SerialPortActions::get_subaru_04_32bit_kernel_baudrate(void)
{
    return runOnBackend([this] { return m_backend->get_subaru_04_32bit_kernel_baudrate(); });
}
bool SerialPortActions::set_subaru_04_32bit_kernel_baudrate(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_subaru_04_32bit_kernel_baudrate(value); });
}
QString SerialPortActions::get_subaru_05_32bit_kernel_baudrate(void)
{
    return runOnBackend([this] { return m_backend->get_subaru_05_32bit_kernel_baudrate(); });
}
bool SerialPortActions::set_subaru_05_32bit_kernel_baudrate(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_subaru_05_32bit_kernel_baudrate(value); });
}

QString SerialPortActions::get_can_speed(void)
{
    return runOnBackend([this] { return m_backend->get_can_speed(); });
}
bool SerialPortActions::set_can_speed(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_can_speed(value); });
}
uint8_t SerialPortActions::get_serial_port_parity(void)
{
    return runOnBackend([this] { return m_backend->get_serial_port_parity(); });
}
bool SerialPortActions::set_serial_port_parity(uint8_t parity)
{
    return runOnBackend([this, parity] { return m_backend->set_serial_port_parity(parity); });
}
QString SerialPortActions::get_serial_port_baudrate(void)
{
    return runOnBackend([this] { return m_backend->get_serial_port_baudrate(); });
}
bool SerialPortActions::set_serial_port_baudrate(QString value)
{
    emit LOG_D("Setting serialport baudrate in SerialPortActions", true, true);
    return runOnBackend([this, value] { return m_backend->set_serial_port_baudrate(value); });
}
QString SerialPortActions::get_serial_port_linux(void)
{
    return runOnBackend([this] { return m_backend->get_serial_port_linux(); });
}
bool SerialPortActions::set_serial_port_linux(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_serial_port_linux(value); });
}
QString SerialPortActions::get_serial_port_windows(void)
{
    return runOnBackend([this] { return m_backend->get_serial_port_windows(); });
}
bool SerialPortActions::set_serial_port_windows(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_serial_port_windows(value); });
}
QString SerialPortActions::get_serial_port(void)
{
    return runOnBackend([this] { return m_backend->get_serial_port(); });
}
bool SerialPortActions::set_serial_port(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_serial_port(value); });
}
QString SerialPortActions::get_serial_port_prefix(void)
{
    return runOnBackend([this] { return m_backend->get_serial_port_prefix(); });
}
bool SerialPortActions::set_serial_port_prefix(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_serial_port_prefix(value); });
}
QString SerialPortActions::get_serial_port_prefix_linux(void)
{
    return runOnBackend([this] { return m_backend->get_serial_port_prefix_linux(); });
}
bool SerialPortActions::set_serial_port_prefix_linux(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_serial_port_prefix_linux(value); });
}
QString SerialPortActions::get_serial_port_prefix_win(void)
{
    return runOnBackend([this] { return m_backend->get_serial_port_prefix_win(); });
}
bool SerialPortActions::set_serial_port_prefix_win(QString value)
{
    return runOnBackend([this, value] { return m_backend->set_serial_port_prefix_win(value); });
}

uint32_t SerialPortActions::get_can_source_address(void)
{
    return runOnBackend([this] { return m_backend->get_can_source_address(); });
}
bool SerialPortActions::set_can_source_address(uint32_t value)
{
    return runOnBackend([this, value] { return m_backend->set_can_source_address(value); });
}
uint32_t SerialPortActions::get_can_destination_address(void)
{
    return runOnBackend([this] { return m_backend->get_can_destination_address(); });
}
bool SerialPortActions::set_can_destination_address(uint32_t value)
{
    return runOnBackend([this, value] { return m_backend->set_can_destination_address(value); });
}
uint32_t SerialPortActions::get_iso15765_source_address(void)
{
    return runOnBackend([this] { return m_backend->get_iso15765_source_address(); });
}
bool SerialPortActions::set_iso15765_source_address(uint32_t value)
{
    return runOnBackend([this, value] { return m_backend->set_iso15765_source_address(value); });
}
uint32_t SerialPortActions::get_iso15765_destination_address(void)
{
    return runOnBackend([this] { return m_backend->get_iso15765_destination_address(); });
}
bool SerialPortActions::set_iso15765_destination_address(uint32_t value)
{
    return runOnBackend([this, value] { return m_backend->set_iso15765_destination_address(value); });
}

// -- operations ---------------------------------------------------------

bool SerialPortActions::is_serial_port_open()
{
    return runOnBackend([this] { return m_backend->is_serial_port_open(); });
}

int SerialPortActions::change_port_speed(QString portSpeed)
{
    set_comm_busy(true);
    int result = runOnBackend([this, portSpeed] { return m_backend->change_port_speed(portSpeed); });
    set_comm_busy(false);
    return result;
}

bool SerialPortActions::set_kline_timings(uint32_t parameter, int value)
{
    return runOnBackend([this, parameter, value] { return m_backend->set_kline_timings(parameter, value); });
}

int SerialPortActions::set_j2534_ioctl(uint32_t parameter, int value)
{
    set_comm_busy(true);
    return runOnBackend([this, parameter, value] { return m_backend->set_j2534_ioctl(parameter, value); });
}

QByteArray SerialPortActions::five_baud_init(QByteArray output)
{
    set_comm_busy(true);
    return runOnBackend([this, output] { return m_backend->five_baud_init(output); });
}

int SerialPortActions::fast_init(QByteArray output)
{
    set_comm_busy(true);
    return runOnBackend([this, output] { return m_backend->fast_init(output); });
}

int SerialPortActions::set_lec_lines(int lec1, int lec2)
{
    set_comm_busy(true);
    int result = runOnBackend([this, lec1, lec2] { return m_backend->set_lec_lines(lec1, lec2); });
    set_comm_busy(false);
    return result;
}

int SerialPortActions::pulse_lec_1_line(int timeout)
{
    set_comm_busy(true);
    int result = runOnBackend([this, timeout] { return m_backend->pulse_lec_1_line(timeout); });
    set_comm_busy(false);
    return result;
}

int SerialPortActions::pulse_lec_2_line(int timeout)
{
    set_comm_busy(true);
    int result = runOnBackend([this, timeout] { return m_backend->pulse_lec_2_line(timeout); });
    set_comm_busy(false);
    return result;
}

bool SerialPortActions::get_is_comm_busy()
{
    return is_comm_busy;
}

void SerialPortActions::set_comm_busy(bool value)
{
    is_comm_busy = value;
}

bool SerialPortActions::get_read_vbatt()
{
    return is_read_vbatt;
}

void SerialPortActions::set_read_vbatt(bool value)
{
    is_read_vbatt = value;
}

bool SerialPortActions::reset_connection()
{
    runOnBackend([this] { m_backend->reset_connection(); });
    return true;
}

QByteArray SerialPortActions::read_serial_obd_data(uint16_t timeout)
{
    QByteArray response = runOnBackend([this, timeout] {
        return m_backend->read_serial_obd_data(timeout);
    });
    emit LOG_D("Response: " + parse_message_to_hex(response.mid(0, 20)), true, true);
    set_comm_busy(false);
    return response;
}

QByteArray SerialPortActions::read_serial_data(uint16_t timeout)
{
    QByteArray response = runOnBackend([this, timeout] {
        QByteArray r = m_backend->read_serial_data(timeout);
        if (get_read_vbatt())
        {
            vBatt.store(m_backend->read_vbatt());
            set_read_vbatt(false);
        }
        return r;
    });
    emit LOG_D("Response: " + parse_message_to_hex(response.mid(0, 20)), true, true);
    set_comm_busy(false);
    return response;
}

QByteArray SerialPortActions::write_serial_data(QByteArray output)
{
    set_comm_busy(true);
    emit LOG_D("Sent: " + parse_message_to_hex(output.mid(0, 20)), true, true);
    return runOnBackend([this, output] { return m_backend->write_serial_data(output); });
}

QByteArray SerialPortActions::write_serial_data_echo_check(QByteArray output)
{
    set_comm_busy(true);
    emit LOG_D("Sent: " + parse_message_to_hex(output.mid(0, 20)), true, true);
    return runOnBackend([this, output] { return m_backend->write_serial_data_echo_check(output); });
}

bool SerialPortActions::get_is_tx_done()
{
    return runOnBackend([this] { return m_backend->get_is_tx_done(); });
}

int SerialPortActions::clear_rx_buffer()
{
    set_comm_busy(true);
    int result = runOnBackend([this] { return m_backend->clear_rx_buffer(); });
    set_comm_busy(false);
    return result;
}

int SerialPortActions::clear_tx_buffer()
{
    set_comm_busy(true);
    int result = runOnBackend([this] { return m_backend->clear_tx_buffer(); });
    set_comm_busy(false);
    return result;
}

int SerialPortActions::send_periodic_j2534_data(QByteArray output, int timeout)
{
    set_comm_busy(true);
    int result = runOnBackend([this, output, timeout] { return m_backend->send_periodic_j2534_data(output, timeout); });
    set_comm_busy(false);
    return result;
}

int SerialPortActions::stop_periodic_j2534_data()
{
    set_comm_busy(true);
    int result = runOnBackend([this] { return m_backend->stop_periodic_j2534_data(); });
    set_comm_busy(false);
    return result;
}

QStringList SerialPortActions::check_serial_ports()
{
    return runOnBackend([this] { return m_backend->check_serial_ports(); });
}

QString SerialPortActions::open_serial_port()
{
    return runOnBackend([this] { return m_backend->open_serial_port(); });
}

unsigned long SerialPortActions::read_vbatt()
{
    if (!get_is_comm_busy() && !get_read_vbatt())
        vBatt.store(runOnBackend([this] { return m_backend->read_vbatt(); }));
    else
        set_read_vbatt(true);
    return vBatt.load();
}

QString SerialPortActions::parse_message_to_hex(QByteArray received)
{
    QString msg;

    for (int i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1 ").arg((uint8_t)received.at(i),2,16,QLatin1Char('0')).toUtf8());
    }

    return msg;
}
