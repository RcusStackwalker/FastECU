#pragma once

#include <QObject>
#include <QRemoteObjectReplica>
#include <QString>
#include <QStringList>

#include <optional>

class SerialPortActions;

namespace fastecu::desktop::connection
{

// The toolbar's log-transport combo text, parsed once.
enum class LogTransport
{
    Can,
    Iso15765,
    KLine,
    Ssm,
    Other,
};

LogTransport log_transport_from_text(const QString& text);

// MainWindow's view of the adapter: the port list, opening and resetting,
// the log-transport flag profile, the two idle resets, and battery voltage.
// Each member reproduces the facade call sequence MainWindow made before
// step 6h. Non-owning: the facade must outlive this object.
class AdapterConnection final : public QObject
{
    Q_OBJECT

  public:
    explicit AdapterConnection(SerialPortActions& facade, QObject *parent = nullptr);

    QStringList available_ports();
    void set_initial_port(const QString& port, const QString& baud);
    void select_port(const QString& port);
    // The opened port's name; empty when nothing opened.
    QString open();
    bool is_open();
    void reset();
    void apply_log_transport(LogTransport transport, bool ssm_protocol);
    // check_serial_ports' reset: every link flag cleared, 4800 baud. Parity
    // is left alone.
    void clear_link_flags();
    // disconnect_from_ecu's reset: 4800 baud, no parity. The link flags are
    // left alone, because connect_to_ecu does not reapply them.
    void return_to_idle();
    void set_port_speed(int baud);
    // Empty unless the adapter is an OpenPort, the only one that reports it.
    std::optional<unsigned long> battery_millivolts();
    void wait_for_source();
    // For the handoffs that still take the facade: the flash controller, the
    // diagnostic link, and reset_serial_to_idle.
    SerialPortActions& facade();

  signals:
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);

  private:
    SerialPortActions& facade_;
};

} // namespace fastecu::desktop::connection
