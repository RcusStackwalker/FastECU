#include "flash_operation_worker.h"

FlashOperationWorker::FlashOperationWorker(QWidget *dialog, QObject *parent, PromptFn promptOverride)
    : QThread(parent)
    , m_dialog(dialog)
    , m_promptOverride(std::move(promptOverride))
{
    if (!m_promptOverride)
    {
        m_promptOverride = [](QWidget *dialog, QString title, QString text,
                               int buttons, int defaultButton) {
            return static_cast<int>(QMessageBox::warning(dialog, title, text,
                                     QMessageBox::StandardButtons(buttons),
                                     QMessageBox::StandardButton(defaultButton)));
        };
    }
}

void FlashOperationWorker::requestStop()
{
    m_stopRequested.storeRelaxed(true);
}

bool FlashOperationWorker::stopRequested() const
{
    return m_stopRequested.loadRelaxed();
}

void FlashOperationWorker::run()
{
    bool success = execute();
    emit operationFinished(success);
}

int FlashOperationWorker::confirm(const QString &title, const QString &text,
                                   QMessageBox::StandardButtons buttons,
                                   QMessageBox::StandardButton defaultButton)
{
    int result = static_cast<int>(defaultButton);
    PromptFn prompt = m_promptOverride;
    QWidget *dialog = m_dialog;
    QMetaObject::invokeMethod(dialog, [prompt, dialog, title, text, buttons, defaultButton]() {
        return prompt(dialog, title, text, int(buttons), int(defaultButton));
    }, Qt::BlockingQueuedConnection, &result);
    return result;
}

void FlashOperationWorker::delay(int timeout)
{
    QThread::msleep(timeout);
}
