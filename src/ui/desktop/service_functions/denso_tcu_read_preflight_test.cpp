#include "src/ui/desktop/service_functions/denso_tcu_read_preflight.h"

#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTest>
#include <QTimer>

#include <gmock/gmock.h>

#include <memory>
#include <utility>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/ui/desktop/service_functions/service_function_dialog.h"

namespace fastecu::service_functions
{
namespace
{

constexpr auto kChooserText = "Choose which option";
constexpr auto kChooserInformation = "Perform TCU ROM Dump, Relearn, Read Parmeter or Set Parameter?";
constexpr auto kIgnitionText = "Turn ignition ON and press OK to start initializing connection to TCU";

class ChooserDriver final : public QObject
{
    Q_OBJECT

  public:
    explicit ChooserDriver(QString choice) : choice_(std::move(choice))
    {
        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, &ChooserDriver::drive);
    }

    void start()
    {
        elapsed_.start();
        timer_.start();
    }

    bool sawChooser() const
    {
        return saw_chooser_;
    }

    bool timedOut() const
    {
        return timed_out_;
    }

    QString text() const
    {
        return text_;
    }

    QString information() const
    {
        return information_;
    }

    QStringList buttonLabels() const
    {
        return button_labels_;
    }

  private slots:
    void drive()
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *message_box = qobject_cast<QMessageBox *>(widget);
            if (message_box == nullptr || message_box->text() != kChooserText)
            {
                continue;
            }

            saw_chooser_ = true;
            text_ = message_box->text();
            information_ = message_box->informativeText();
            for (QAbstractButton *button : message_box->buttons())
            {
                button_labels_.push_back(button->text());
            }
            button_labels_.sort();

            if (choice_.isEmpty())
            {
                message_box->reject();
                return;
            }
            for (QAbstractButton *button : message_box->buttons())
            {
                if (button->text() == choice_)
                {
                    button->click();
                    return;
                }
            }
        }

        if (elapsed_.elapsed() > 2000)
        {
            timed_out_ = true;
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
    QString choice_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    bool saw_chooser_ = false;
    bool timed_out_ = false;
    QString text_;
    QString information_;
    QStringList button_labels_;
};

class ServiceActionDriver final : public QObject
{
    Q_OBJECT

  public:
    explicit ServiceActionDriver(bool accept_ignition) : accept_ignition_(accept_ignition)
    {
        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, &ServiceActionDriver::drive);
    }

    void start()
    {
        elapsed_.start();
        timer_.start();
    }

    int ignitionCount() const
    {
        return ignition_count_;
    }

    QMessageBox::Icon ignitionIcon() const
    {
        return ignition_icon_;
    }

    QString ignitionText() const
    {
        return ignition_text_;
    }

    QMessageBox::StandardButtons ignitionButtons() const
    {
        return ignition_buttons_;
    }

    QStringList serviceDialogTitles() const
    {
        return service_dialog_titles_;
    }

    bool timedOut() const
    {
        return timed_out_;
    }

  private slots:
    void drive()
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *message_box = qobject_cast<QMessageBox *>(widget);
                message_box != nullptr && message_box->text() == kIgnitionText)
            {
                ++ignition_count_;
                ignition_icon_ = message_box->icon();
                ignition_text_ = message_box->text();
                ignition_buttons_ = message_box->standardButtons();
                message_box->done(accept_ignition_ ? QMessageBox::Ok : QMessageBox::Cancel);
                return;
            }
            if (auto *dialog = qobject_cast<ServiceFunctionDialog *>(widget); dialog != nullptr)
            {
                service_dialog_titles_.push_back(dialog->windowTitle());
                dialog->reject();
                return;
            }
        }

        if (elapsed_.elapsed() > 2000)
        {
            timed_out_ = true;
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
    bool accept_ignition_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    int ignition_count_ = 0;
    QMessageBox::Icon ignition_icon_ = QMessageBox::NoIcon;
    QString ignition_text_;
    QMessageBox::StandardButtons ignition_buttons_ = QMessageBox::NoButton;
    QStringList service_dialog_titles_;
    bool timed_out_ = false;
};

std::unique_ptr<SerialPortActions> recordingSerial(FakeBackend **fake)
{
    auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                      [fake]() -> SerialBackend *
                                                      {
                                                          *fake = new NiceFakeBackend;
                                                          return *fake;
                                                      });
    if (!serial->set_add_ssm_header(false) || *fake == nullptr)
    {
        return nullptr;
    }
    return serial;
}

void expectNoBackendIo(FakeBackend& fake)
{
    EXPECT_CALL(fake, is_serial_port_open()).Times(0);
    EXPECT_CALL(fake, reset_connection()).Times(0);
    EXPECT_CALL(fake, change_port_speed(::testing::_)).Times(0);
    EXPECT_CALL(fake, open_serial_port()).Times(0);
    EXPECT_CALL(fake, read_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(fake, read_vbatt()).Times(0);
}

} // namespace

class DensoTcuReadPreflightTest : public QObject
{
    Q_OBJECT

  private slots:
    void chooserReturnsTheActionNamedByEachLegacyButton_data()
    {
        QTest::addColumn<QString>("choice");
        QTest::addColumn<int>("expected_action");
        QTest::newRow("dump") << "Dump" << static_cast<int>(DensoTcuReadAction::Dump);
        QTest::newRow("relearn") << "Relearn" << static_cast<int>(DensoTcuReadAction::Relearn);
        QTest::newRow("read") << "Read Param" << static_cast<int>(DensoTcuReadAction::ReadParameters);
        QTest::newRow("set") << "Set Param" << static_cast<int>(DensoTcuReadAction::SetParameters);
    }

    void chooserReturnsTheActionNamedByEachLegacyButton()
    {
        QFETCH(QString, choice);
        QFETCH(int, expected_action);

        ChooserDriver driver{choice};
        driver.start();
        const DensoTcuReadAction action = choose_denso_tcu_read_action(nullptr);

        QVERIFY(driver.sawChooser());
        QVERIFY(!driver.timedOut());
        QCOMPARE(driver.text(), QString(kChooserText));
        QCOMPARE(driver.information(), QString(kChooserInformation));
        QCOMPARE(driver.buttonLabels(), QStringList({"Dump", "Read Param", "Relearn", "Set Param"}));
        QCOMPARE(static_cast<int>(action), expected_action);
    }

    void dismissingChooserReturnsCancelled()
    {
        ChooserDriver driver{{}};
        driver.start();

        QCOMPARE(choose_denso_tcu_read_action(nullptr), DensoTcuReadAction::Cancelled);
        QVERIFY(driver.sawChooser());
        QVERIFY(!driver.timedOut());
    }

    void dumpAndCancelledReturnWithoutIgnitionOrSerialCalls_data()
    {
        QTest::addColumn<int>("action");
        QTest::addColumn<bool>("handled");
        QTest::newRow("dump") << static_cast<int>(DensoTcuReadAction::Dump) << false;
        QTest::newRow("cancelled") << static_cast<int>(DensoTcuReadAction::Cancelled) << true;
    }

    void dumpAndCancelledReturnWithoutIgnitionOrSerialCalls()
    {
        QFETCH(int, action);
        QFETCH(bool, handled);
        FakeBackend *fake = nullptr;
        auto serial = recordingSerial(&fake);
        QVERIFY(serial != nullptr);
        expectNoBackendIo(*fake);

        ServiceActionDriver driver{false};
        driver.start();
        QCOMPARE(run_denso_tcu_service_action(static_cast<DensoTcuReadAction>(action), serial.get(),
                                              "sub_tcu_denso_sh7058_can", nullptr),
                 handled);
        QTest::qWait(20);

        QCOMPARE(driver.ignitionCount(), 0);
        QVERIFY(driver.serviceDialogTitles().isEmpty());
    }

    void decliningIgnitionSkipsEveryServiceDialogAndSerialCall_data()
    {
        QTest::addColumn<int>("action");
        QTest::newRow("relearn") << static_cast<int>(DensoTcuReadAction::Relearn);
        QTest::newRow("read") << static_cast<int>(DensoTcuReadAction::ReadParameters);
        QTest::newRow("set") << static_cast<int>(DensoTcuReadAction::SetParameters);
    }

    void decliningIgnitionSkipsEveryServiceDialogAndSerialCall()
    {
        QFETCH(int, action);
        FakeBackend *fake = nullptr;
        auto serial = recordingSerial(&fake);
        QVERIFY(serial != nullptr);
        expectNoBackendIo(*fake);

        ServiceActionDriver driver{false};
        driver.start();
        QVERIFY(run_denso_tcu_service_action(static_cast<DensoTcuReadAction>(action), serial.get(),
                                             "sub_tcu_denso_sh7058_can", nullptr));

        QVERIFY(!driver.timedOut());
        QCOMPARE(driver.ignitionCount(), 1);
        QCOMPARE(driver.ignitionIcon(), QMessageBox::Warning);
        QCOMPARE(driver.ignitionText(), QString(kIgnitionText));
        QCOMPARE(driver.ignitionButtons(), QMessageBox::Ok | QMessageBox::Cancel);
        QVERIFY(driver.serviceDialogTitles().isEmpty());
    }

    void acceptingIgnitionOpensTheMatchingRealServiceDialog_data()
    {
        QTest::addColumn<int>("action");
        QTest::addColumn<QString>("title");
        QTest::newRow("relearn") << static_cast<int>(DensoTcuReadAction::Relearn) << "TCU Relearn";
        QTest::newRow("read") << static_cast<int>(DensoTcuReadAction::ReadParameters) << "Read TCU Parameters";
        QTest::newRow("set") << static_cast<int>(DensoTcuReadAction::SetParameters) << "Set TCU Parameters";
    }

    void acceptingIgnitionOpensTheMatchingRealServiceDialog()
    {
        QFETCH(int, action);
        QFETCH(QString, title);
        FakeBackend *fake = nullptr;
        auto serial = recordingSerial(&fake);
        QVERIFY(serial != nullptr);
        expectNoBackendIo(*fake);

        ServiceActionDriver driver{true};
        driver.start();
        QVERIFY(run_denso_tcu_service_action(static_cast<DensoTcuReadAction>(action), serial.get(),
                                             "sub_tcu_denso_sh7058_can", nullptr));

        QVERIFY(!driver.timedOut());
        QCOMPARE(driver.ignitionCount(), 1);
        QCOMPARE(driver.ignitionIcon(), QMessageBox::Warning);
        QCOMPARE(driver.ignitionText(), QString(kIgnitionText));
        QCOMPARE(driver.ignitionButtons(), QMessageBox::Ok | QMessageBox::Cancel);
        QCOMPARE(driver.serviceDialogTitles(), QStringList({title}));
    }
};

} // namespace fastecu::service_functions

QTEST_MAIN(fastecu::service_functions::DensoTcuReadPreflightTest)
#include "denso_tcu_read_preflight_test.moc"
