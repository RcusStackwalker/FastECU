#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QApplication>
#include <QButtonGroup>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSerialPort>
#include <QSpacerItem>
#include <QTextEdit>
#include <QTime>
#include <QTimer>
#include <QWidget>

#include <ui_data_terminal.h>

#include "src/backend/diagnostics/terminal_script.h"
#include "src/backend/protocol/idiagnostic_link.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
class DataTerminalWindow;
}
QT_END_NAMESPACE

class DataTerminal : public QDialog
{
    Q_OBJECT

  signals:
    void logE(QString message, bool timestamp, bool linefeed);
    void logW(QString message, bool timestamp, bool linefeed);
    void logI(QString message, bool timestamp, bool linefeed);
    void logD(QString message, bool timestamp, bool linefeed);

  public:
    explicit DataTerminal(fastecu::diagnostics::IDiagnosticLink& link, QWidget *parent = nullptr);
    ~DataTerminal();

  private:
    uint16_t serial_read_short_timeout_ = 200;

    QVBoxLayout *v_box_layout_{};

    uint8_t calculateChecksum(const QByteArray& output, bool dec0x100);
    QByteArray addSsmHeader(QByteArray output, uint8_t testerId, uint8_t targetId, bool dec0x100);
    QString parseMessageToHex(const QByteArray& received);
    void delay(int timeout);
    void warn(const QString& message);
    std::optional<std::vector<fastecu::diagnostics::TerminalStep>> readScript(const QString& msg);
    template <typename Config> void openLink(bool settingsOk, const Config& config);
    void sendKline(const std::vector<fastecu::diagnostics::TerminalStep>& script);
    void sendCan(const std::vector<fastecu::diagnostics::TerminalStep>& script);
    void runScript(const std::vector<fastecu::diagnostics::TerminalStep>& script,
                   const std::function<QByteArray(const bytes::Bytes&)>& encode);

    fastecu::diagnostics::IDiagnosticLink *link_ = nullptr;

  signals:

  private slots:
    void protocolTypeChanged(int);
    void listenInterface();
    void sendToInterface();

  private:
    std::unique_ptr<Ui::DataTerminalWindow> ui_;
};
