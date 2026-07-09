#include <QtTest>
#include <cstdio>
#include <QApplication>
#include <QSignalSpy>
#include <QWidget>
#include "modules/flash_operation_worker.h"
#include "test_flash_operation_worker.h"

class ScriptedOperation : public FlashOperationWorker
{
    Q_OBJECT
  public:
    using FlashOperationWorker::FlashOperationWorker;

    bool succeedResult = true;
    bool checkStopBeforeReturning = false;
    int confirmResultSeen = -1;

  protected:
    bool execute() override
    {
        if (checkStopBeforeReturning)
        {
            while (!stopRequested())
                QThread::msleep(5);
            return false;
        }
        emit LOG_I("scripted operation running", true, true);
        emit progressChanged(50);
        confirmResultSeen = confirm("title", "text",
                                    QMessageBox::Yes | QMessageBox::Cancel,
                                    QMessageBox::Cancel);
        return succeedResult;
    }
};

class TestFlashOperationWorker : public QObject
{
    Q_OBJECT
  private:
    static FlashOperationWorker::PromptFn yesPrompt()
    {
        return [](QWidget *, QString, QString, int, int) -> int
        {
            return QMessageBox::Yes;
        };
    }

  private slots:
    void run_emitsOperationFinished_withExecuteResult()
    {
        QWidget dialog;
        ScriptedOperation op(&dialog, nullptr, yesPrompt());
        op.succeedResult = true;
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(2000));
        QCOMPARE(finishedSpy.size(), 1);
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
        QVERIFY(op.wait(2000));
    }

    void requestStop_isNoticedByExecute()
    {
        QWidget dialog;
        ScriptedOperation op(&dialog);
        op.checkStopBeforeReturning = true;
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QTest::qWait(20);
        op.requestStop();
        QVERIFY(finishedSpy.wait(2000));
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
        QVERIFY(op.wait(2000));
    }

    void confirm_runsPromptOnDialogThread_andReturnsItsAnswer()
    {
        QWidget dialog;
        QThread *callingThread = nullptr;
        auto prompt = [&callingThread](QWidget *, QString, QString, int, int) -> int
        {
            callingThread = QThread::currentThread();
            return QMessageBox::Yes;
        };
        ScriptedOperation op(&dialog, nullptr, prompt);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));

        QCOMPARE(op.confirmResultSeen, int(QMessageBox::Yes));
        QCOMPARE(callingThread, dialog.thread());
    }

    void logAndProgressSignals_arriveAtCallingThread()
    {
        QWidget dialog;
        ScriptedOperation op(&dialog, nullptr, yesPrompt());
        QSignalSpy logSpy(&op, &FlashOperationWorker::LOG_I);
        QSignalSpy progressSpy(&op, &FlashOperationWorker::progressChanged);
        // execute() calls confirm(), which blocks on a BlockingQueuedConnection
        // invoke back onto this (the dialog's) thread -- so we must pump this
        // thread's event loop while waiting, rather than plain QThread::wait(),
        // or the confirm() call deadlocks against us.
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));

        QTRY_COMPARE(logSpy.size(), 1);
        QCOMPARE(progressSpy.size(), 1);
        QCOMPARE(progressSpy.at(0).at(0).toInt(), 50);
    }
};

int run_test_flash_operation_worker(int argc, char **argv)
{
    // ScriptedOperation constructs a QWidget (the dialog), which requires a
    // QApplication rather than a plain QCoreApplication to construct (even
    // though we never show a widget).
    fprintf(stderr, "[diag] QT_QPA_PLATFORM='%s'\n", qEnvironmentVariable("QT_QPA_PLATFORM").toUtf8().constData());
    fflush(stderr);
    fprintf(stderr, "[diag] before QApplication construction\n");
    fflush(stderr);
    QApplication app(argc, argv);
    fprintf(stderr, "[diag] after QApplication construction\n");
    fflush(stderr);
    TestFlashOperationWorker t;
    fprintf(stderr, "[diag] before QTest::qExec\n");
    fflush(stderr);
    return QTest::qExec(&t, argc, argv);
}
#include "test_flash_operation_worker.moc"
