#include "src/ui/desktop/flash/tcu/flash_tcu_subaru_denso_sh705x_can.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QTest>

#include <memory>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

namespace
{

class PreflightDriver final : public QObject
{
    Q_OBJECT

  public:
    explicit PreflightDriver(QString choice) : choice_(std::move(choice))
    {
        timer_.setInterval(10);
        connect(&timer_, &QTimer::timeout, this, &PreflightDriver::drive);
    }

    void start()
    {
        elapsed_.start();
        timer_.start();
    }

    bool saw_preflight() const
    {
        return saw_preflight_;
    }

    bool selected_choice() const
    {
        return selected_choice_;
    }

    QString preflight_text() const
    {
        return preflight_text_;
    }

  private slots:
    void drive()
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *message_box = qobject_cast<QMessageBox *>(widget);
            if (message_box == nullptr)
            {
                continue;
            }
            if (!selected_choice_ && message_box->text() == "Choose which option")
            {
                if (QPushButton *button = findButton(*message_box, choice_); button != nullptr)
                {
                    selected_choice_ = true;
                    button->click();
                }
                return;
            }
            if (message_box->text() == "Turn ignition ON and press OK to start initializing connection to TCU")
            {
                saw_preflight_ = true;
                preflight_text_ = message_box->text();
                message_box->done(QMessageBox::Cancel);
                return;
            }
        }

        // Before the dispatch fix, service-function selections opened their
        // own modal dialog and never reached the ignition warning. Close that
        // dialog on timeout so this regression test fails rather than hangs.
        if (elapsed_.elapsed() > 1000)
        {
            for (QWidget *widget : QApplication::topLevelWidgets())
            {
                if (auto *dialog = qobject_cast<QDialog *>(widget); dialog != nullptr)
                {
                    dialog->reject();
                }
            }
        }
    }

  private:
    static QPushButton *findButton(const QMessageBox& message_box, const QString& text)
    {
        for (QAbstractButton *button : message_box.buttons())
        {
            if (button->text() == text)
            {
                return qobject_cast<QPushButton *>(button);
            }
        }
        return nullptr;
    }

    QString choice_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    bool selected_choice_ = false;
    bool saw_preflight_ = false;
    QString preflight_text_;
};

} // namespace

class FlashTcuSubaruDensoSH705xCanTest : public QObject
{
    Q_OBJECT

  private slots:
    void cancellingIgnitionPreflightPreventsEveryReadActionFromConfiguringIo_data()
    {
        QTest::addColumn<QString>("choice");
        QTest::newRow("dump") << "Dump";
        QTest::newRow("relearn") << "Relearn";
        QTest::newRow("read-parameters") << "Read Param";
        QTest::newRow("set-parameters") << "Set Param";
    }

    void cancellingIgnitionPreflightPreventsEveryReadActionFromConfiguringIo()
    {
        QFETCH(QString, choice);

        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend;
                                                              return fake;
                                                          });
        QVERIFY(serial->set_add_ssm_header(false));
        QVERIFY(fake != nullptr);
        fake->logLifecycleCalls = true;
        fake->takeCallLog();

        FileActions::EcuCalDefStructure definition;
        definition.FlashMethod = "sub_tcu_denso_sh7058_can";
        FlashTcuSubaruDensoSH705xCan dialog{serial.get(), &definition, "read"};
        PreflightDriver driver{choice};
        driver.start();

        dialog.run();

        QVERIFY(driver.selected_choice());
        QVERIFY(driver.saw_preflight());
        QCOMPARE(driver.preflight_text(),
                 QString("Turn ignition ON and press OK to start initializing connection to TCU"));
        QVERIFY(fake->takeCallLog().isEmpty());
    }
};

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    FlashTcuSubaruDensoSH705xCanTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "flash_tcu_subaru_denso_sh705x_can_test.moc"
