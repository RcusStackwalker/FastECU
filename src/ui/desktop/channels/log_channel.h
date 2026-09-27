#pragma once

#include <QObject>
#include <QString>

namespace fastecu::ui
{

// The UI's system-log endpoint. DesktopComposition connects it to the desktop
// system logger; UI objects connect their own LOG_* signals to the signals of
// the same name here. The names are part of the contract: the logger reads a
// line's level from the name of the signal that delivered it. Relaying
// through this long-lived object also keeps a line whose original sender is
// destroyed before the logger's thread delivers it.
class LogChannel final : public QObject
{
    Q_OBJECT

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);
    void enable_log_write_to_file(bool enable);
    // Formatted non-debug lines for the log window, from the logger.
    void log_window_message(QString message);
};

} // namespace fastecu::ui
