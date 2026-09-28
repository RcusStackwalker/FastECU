#include <QApplication>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QTimer>
#include <QDir>
#include <QTest>

#include "apps/desktop/default_config_root.h"
#include "apps/desktop/startup_diagnostics.h"
#include "apps/desktop/startup_event_sink.h"
#include "apps/desktop/startup_test_platform.h"

namespace
{
class MessageCapture
{
  public:
    MessageCapture() : previous_(qInstallMessageHandler(handler))
    {
        active_ = this;
    }
    ~MessageCapture()
    {
        qInstallMessageHandler(previous_);
        active_ = nullptr;
    }
    QList<QtMsgType> levels;
    QStringList texts;

  private:
    static void handler(QtMsgType level, const QMessageLogContext& context, const QString& text)
    {
        if (level == QtWarningMsg || level == QtCriticalMsg)
        {
            active_->levels.append(level);
            active_->texts.append(text);
        }
        else if (active_->previous_)
        {
            active_->previous_(level, context, text);
        }
    }
    QtMessageHandler previous_;
    inline static MessageCapture *active_ = nullptr;
};
class StartupModalDriver
{
  public:
    StartupModalDriver()
    {
        elapsed_.start();
        QObject::connect(&timer_, &QTimer::timeout,
                         [this]
                         {
                             auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                             if (dialog == nullptr)
                             {
                                 return;
                             }
                             if (auto *box = qobject_cast<QMessageBox *>(dialog))
                             {
                                 titles.append(box->windowTitle());
                                 texts.append(box->text());
                                 icons.append(box->icon());
                                 box->accept();
                             }
                             else
                             {
                                 unexpected = true;
                                 dialog->reject();
                             }
                             if (elapsed_.elapsed() > 3000)
                             {
                                 timed_out = true;
                                 dialog->reject();
                             }
                         });
        timer_.start(5);
    }
    QStringList titles;
    QStringList texts;
    QList<QMessageBox::Icon> icons;
    bool unexpected = false;
    bool timed_out = false;

  private:
    QTimer timer_;
    QElapsedTimer elapsed_;
};
} // namespace

class StartupDiagnosticsTest : public QObject
{
    Q_OBJECT

  private slots:
    void realPresentersReportSeverityAndOrderedDetails()
    {
        StartupModalDriver dialogs;
        MessageCapture messages;
        const QString detail = QStringLiteral("Invalid /tmp/配置/protocols.cfg: broken");
        present_startup_failure({fastecu::ErrorKind::InvalidConfig, detail.toStdString()});
        present_startup_warnings({"first /a.cfg", "second /b.cfg"});
        QCOMPARE(dialogs.icons, (QList<QMessageBox::Icon>{QMessageBox::Critical, QMessageBox::Warning}));
        QCOMPARE(dialogs.titles, (QStringList{startup_message_box_title(), startup_message_box_title()}));
        QVERIFY(dialogs.texts[0].contains(detail));
        QVERIFY(dialogs.texts[1].contains("first /a.cfg\nsecond /b.cfg"));
        // Platform plugins may emit their own warnings; compare the presenter's messages.
        int failure = messages.texts.indexOf(dialogs.texts[0]);
        int warning = messages.texts.indexOf(dialogs.texts[1]);
        QVERIFY(failure >= 0);
        QVERIFY(warning > failure);
        QCOMPARE(messages.levels[failure], QtCriticalMsg);
        QCOMPARE(messages.levels[warning], QtWarningMsg);
        QVERIFY(!dialogs.unexpected);
        QVERIFY(!dialogs.timed_out);
    }

    void emptyWarningsDoNotOpenAModalOrLog()
    {
        StartupModalDriver dialogs;
        MessageCapture messages;
        present_startup_warnings({});
        QCoreApplication::processEvents();
        QVERIFY(dialogs.texts.isEmpty());
        QVERIFY(messages.texts.isEmpty());
    }

    void sinkRetainsBoundedUtf8DiagnosticsInOrder()
    {
        StartupEventSink sink;
        const std::string bounded = "\xE8\xAD\xA6\xE5\x91\x8A suffix";
        const std::string_view warning{bounded.data(), std::size_t{6}};
        sink.log(fastecu::LogLevel::Debug, "debug");
        sink.log(fastecu::LogLevel::Info, "info");
        sink.log(fastecu::LogLevel::Warning, warning);
        sink.progress(0, 10);
        sink.log(fastecu::LogLevel::Error, "\xC3\xA9"
                                           "chec");
        sink.notice(std::string_view{"notice ignored", 6});
        sink.progress(10, 10);
        QCOMPARE(sink.warnings(), (QStringList{QString::fromUtf8("\xE8\xAD\xA6\xE5\x91\x8A"),
                                               QString::fromUtf8("\xC3\xA9"
                                                                 "chec"),
                                               "notice"}));
    }

    void failureTextCarriesTheDetail()
    {
        const QString text = startup_failure_text(
            fastecu::Error{fastecu::ErrorKind::InvalidConfig, "Unable to load protocols /r/protocols.cfg: bad"});
        QVERIFY(text.contains("/r/protocols.cfg"));
        QVERIFY(text.contains("bad"));
    }

    void warningTextListsEveryWarning()
    {
        const QString text = startup_warning_text({"first /a.cfg", "second"});
        QVERIFY(text.contains("first /a.cfg"));
        QVERIFY(text.contains("second"));
    }

    void defaultRootIsUnderHomeAndEndsInFastEcu()
    {
        const QString root = default_config_root();
        QVERIFY(root.startsWith(QDir::homePath() + "/"));
        QVERIFY(root.endsWith("/FastECU/"));
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    StartupDiagnosticsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "startup_diagnostics_test.moc"
