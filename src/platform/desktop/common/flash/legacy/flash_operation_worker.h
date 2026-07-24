#ifndef FLASH_OPERATION_WORKER_H
#define FLASH_OPERATION_WORKER_H

#include <QAtomicInteger>
#include <QMessageBox>
#include <QMetaObject>
#include <QThread>
#include <QWidget>
#include <functional>

// Worker-thread half of a flash/eeprom module's dialog+operation split (worker-thread migration).
// Subclasses relocate a module's protocol methods here and implement
// execute(); run() wraps it and emits operationFinished(). confirm() lets
// worker-thread code show a QMessageBox on the GUI thread and block for the
// answer -- every module has at least one mid-operation confirmation prompt
// today, interleaved with blocking I/O in the same function.
class FlashOperationWorker : public QThread
{
    Q_OBJECT

  public:
    // (dialog, title, text, buttons, defaultButton) -> chosen button.
    // Tests substitute this to auto-answer without a real modal; production
    // code uses the default, which shows a real QMessageBox on dialog's thread.
    using PromptFn = std::function<int(QWidget *dialog, QString title, QString text,
                                       int buttons, int defaultButton)>;

    explicit FlashOperationWorker(QWidget *dialog, QObject *parent = nullptr,
                                  PromptFn promptOverride = {});

    void requestStop();
    bool stopRequested() const;

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);
    void externalLoggerMessage(QString message);
    void progressChanged(int value);
    void operationFinished(bool success);

  protected:
    void run() final;
    virtual bool execute() = 0;

    // Shows a QMessageBox on the GUI thread and blocks this (worker) thread
    // until the user answers. Must only be called from this worker's own
    // thread -- calling it from m_dialog's thread deadlocks.
    int confirm(const QString& title, const QString& text,
                QMessageBox::StandardButtons buttons,
                QMessageBox::StandardButton defaultButton);

    void delay(int timeout);

    QWidget *m_dialog;

  private:
    QAtomicInteger<bool> m_stopRequested{false};
    PromptFn m_promptOverride;
};

#endif // FLASH_OPERATION_WORKER_H
