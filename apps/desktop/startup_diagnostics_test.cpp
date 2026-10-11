#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QTimer>
#include <QDir>
#include <gtest/gtest.h>

#include "apps/desktop/default_config_root.h"
#include "apps/desktop/startup_diagnostics.h"
#include "apps/desktop/startup_event_sink.h"
#include "apps/desktop/startup_test_platform.h"

namespace
{
class MessageCapture
{
  public:
    MessageCapture() : previous_(qInstallMessageHandler(Handler))
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
    static void Handler(QtMsgType level, const QMessageLogContext& context, const QString& text)
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

TEST(StartupDiagnosticsTest, realPresentersReportSeverityAndOrderedDetails)
{
    StartupModalDriver dialogs;
    MessageCapture messages;
    const QString detail = QStringLiteral("Invalid /tmp/配置/fastecu.cfg: broken");
    present_startup_failure({fastecu::ErrorKind::kInvalidConfig, detail.toStdString()});
    present_startup_warnings({"first /a.cfg", "second /b.cfg"});
    ASSERT_EQ(dialogs.icons, (QList<QMessageBox::Icon>{QMessageBox::Critical, QMessageBox::Warning}));
    ASSERT_EQ(dialogs.titles, (QStringList{startup_message_box_title(), startup_message_box_title()}));
    ASSERT_TRUE(dialogs.texts[0].contains(detail));
    ASSERT_TRUE(dialogs.texts[1].contains("first /a.cfg\nsecond /b.cfg"));
    // Platform plugins may emit their own warnings; compare the presenter's messages.
    const qsizetype failure = messages.texts.indexOf(dialogs.texts[0]);
    const qsizetype warning = messages.texts.indexOf(dialogs.texts[1]);
    ASSERT_TRUE(failure >= 0);
    ASSERT_TRUE(warning > failure);
    ASSERT_EQ(messages.levels[failure], QtCriticalMsg);
    ASSERT_EQ(messages.levels[warning], QtWarningMsg);
    ASSERT_TRUE(!dialogs.unexpected);
    ASSERT_TRUE(!dialogs.timed_out);
}

TEST(StartupDiagnosticsTest, emptyWarningsDoNotOpenAModalOrLog)
{
    StartupModalDriver dialogs;
    MessageCapture messages;
    present_startup_warnings({});
    QCoreApplication::processEvents();
    ASSERT_TRUE(dialogs.texts.isEmpty());
    ASSERT_TRUE(messages.texts.isEmpty());
}

TEST(StartupDiagnosticsTest, sinkRetainsBoundedUtf8DiagnosticsInOrder)
{
    StartupEventSink sink;
    const std::string bounded = "\xE8\xAD\xA6\xE5\x91\x8A suffix";
    const std::string_view warning{bounded.data(), std::size_t{6}};
    sink.Log(fastecu::LogLevel::kDebug, "debug");
    sink.Log(fastecu::LogLevel::kInfo, "info");
    sink.Log(fastecu::LogLevel::kWarning, warning);
    sink.Progress(0, 10);
    sink.Log(fastecu::LogLevel::kError, "\xC3\xA9"
                                        "chec");
    sink.Notice(std::string_view{"notice ignored", 6});
    sink.Progress(10, 10);
    ASSERT_EQ(sink.warnings(), (QStringList{QString::fromUtf8("\xE8\xAD\xA6\xE5\x91\x8A"),
                                            QString::fromUtf8("\xC3\xA9"
                                                              "chec"),
                                            "notice"}));
}

TEST(StartupDiagnosticsTest, failureTextCarriesTheDetail)
{
    const QString text =
        startup_failure_text(fastecu::Error{fastecu::ErrorKind::kInvalidConfig, "Unable to load /r/fastecu.cfg: bad"});
    ASSERT_TRUE(text.contains("/r/fastecu.cfg"));
    ASSERT_TRUE(text.contains("bad"));
}

TEST(StartupDiagnosticsTest, warningTextListsEveryWarning)
{
    const QString text = startup_warning_text({"first /a.cfg", "second"});
    ASSERT_TRUE(text.contains("first /a.cfg"));
    ASSERT_TRUE(text.contains("second"));
}

TEST(StartupDiagnosticsTest, defaultRootIsUnderHomeAndEndsInFastEcu)
{
    const QString root = default_config_root();
    ASSERT_TRUE(root.startsWith(QDir::homePath() + "/"));
    ASSERT_TRUE(root.endsWith("/FastECU/"));
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment);
}
