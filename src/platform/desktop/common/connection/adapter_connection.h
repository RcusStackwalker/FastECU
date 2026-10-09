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
    kCan,
    kIso15765,
    kKLine,
    kSsm,
    kOther,
};

LogTransport LogTransportFromText(const QString& text);

// MainWindow's view of the adapter: the port list, opening and resetting,
// the log-transport flag profile, the two idle resets, and battery voltage.
// Each member reproduces the facade call sequence MainWindow made before
// step 6h. Non-owning: the facade must outlive this object.
class AdapterConnection final : public QObject
{
    Q_OBJECT

  public:
    explicit AdapterConnection(SerialPortActions& facade, QObject *parent = nullptr);

    QStringList AvailablePorts();
    void SetInitialPort(const QString& port, const QString& baud);
    void SelectPort(const QString& port);
    // The opened port's name; empty when nothing opened.
    QString Open();
    // The name open() last returned, as the facade still holds it; for a
    // caller that opened the port through the diagnostic link instead.
    QString OpenedPort();
    bool IsOpen();
    void Reset();
    void ApplyLogTransport(LogTransport transport, bool ssm_protocol);
    // check_serial_ports' reset: every link flag cleared, 4800 baud. Parity
    // is left alone.
    void ClearLinkFlags();
    // disconnect_from_ecu's reset: 4800 baud, no parity. The link flags are
    // left alone, because connect_to_ecu does not reapply them.
    void ReturnToIdle();
    void SetPortSpeed(int baud);
    // Empty unless the adapter is an OpenPort, the only one that reports it.
    std::optional<unsigned long> BatteryMillivolts();
    void WaitForSource();
    // For the handoffs that still take the facade: the flash controller, the
    // diagnostic link, and reset_serial_to_idle.
    SerialPortActions& Facade();

  signals:
    // NOLINTBEGIN(readability-identifier-naming): Qt signals keep Qt's camelBack names
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State old_state);
    // NOLINTEND(readability-identifier-naming)

  private:
    SerialPortActions& facade_;
};

} // namespace fastecu::desktop::connection
