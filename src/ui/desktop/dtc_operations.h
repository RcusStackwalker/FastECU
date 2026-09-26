#pragma once

#include <QCloseEvent>
#include <QDialog>
#include <QString>

#include <memory>

#include "src/backend/protocol/idiagnostic_link.h"
#include "src/platform/desktop/common/diagnostics/dtc_worker.h"

namespace Ui
{
class DtcOperationsWindow;
}

// Presents OBD-II DTC read/clear. The protocol runs in DtcWorker; the dialog
// collects the protocol choice and forwards log lines.
class DtcOperations : public QDialog
{
    Q_OBJECT

  public:
    explicit DtcOperations(fastecu::diagnostics::IDiagnosticLink& link, QWidget *parent = nullptr);
    ~DtcOperations() override;

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

  private:
    void start(fastecu::diagnostics::DtcOperation operation);
    void forwardLog(int level, const QString& message);
    void finish(const fastecu::diagnostics::DtcWorkerResult& result);
    void setButtonsEnabled(bool enabled);
    // Stops and joins a running worker, then resets the facade. Shared by
    // closeEvent() and reject() (Escape), so neither path can bypass it.
    void stopWorker();

    fastecu::diagnostics::IDiagnosticLink& link_;
    std::unique_ptr<Ui::DtcOperationsWindow> ui;
    std::unique_ptr<fastecu::diagnostics::DtcWorker> worker_;
};
