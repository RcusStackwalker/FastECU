#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <string>
#include "src/platform/desktop/common/testing/event_helpers.h"
#include "src/ui/desktop/service_functions/denso_tcu_read_preflight.h"

#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <gtest/gtest.h>
#include <QTimer>

#include <gmock/gmock.h>

#include <utility>

#include "src/platform/desktop/common/connection/testing/adapter_connection_harness.h"
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

  private:
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

  private:
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

class DensoTcuReadPreflightTest : public ::testing::Test
{

  public:
};

struct chooserReturnsTheActionNamedByEachLegacyButtonCase
{
    std::string name;
    QString choice;
    int expected_action;
};
class chooserReturnsTheActionNamedByEachLegacyButtonParameters
    : public DensoTcuReadPreflightTest,
      public ::testing::WithParamInterface<chooserReturnsTheActionNamedByEachLegacyButtonCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, chooserReturnsTheActionNamedByEachLegacyButtonParameters,
    ::testing::Values(chooserReturnsTheActionNamedByEachLegacyButtonCase{"dump", "Dump",
                                                                         static_cast<int>(DensoTcuReadAction::Dump)},
                      chooserReturnsTheActionNamedByEachLegacyButtonCase{"relearn", "Relearn",
                                                                         static_cast<int>(DensoTcuReadAction::Relearn)},
                      chooserReturnsTheActionNamedByEachLegacyButtonCase{
                          "read", "Read Param", static_cast<int>(DensoTcuReadAction::ReadParameters)},
                      chooserReturnsTheActionNamedByEachLegacyButtonCase{
                          "set", "Set Param", static_cast<int>(DensoTcuReadAction::SetParameters)}),
    [](const ::testing::TestParamInfo<chooserReturnsTheActionNamedByEachLegacyButtonCase>& info)
    { return info.param.name; });

TEST_P(chooserReturnsTheActionNamedByEachLegacyButtonParameters, chooserReturnsTheActionNamedByEachLegacyButton)
{
    const QString choice = GetParam().choice;
    const int expected_action = GetParam().expected_action;

    ChooserDriver driver{choice};
    driver.start();
    const DensoTcuReadAction action = choose_denso_tcu_read_action(nullptr);

    ASSERT_TRUE(driver.sawChooser());
    ASSERT_TRUE(!driver.timedOut());
    ASSERT_EQ(driver.text(), QString(kChooserText));
    ASSERT_EQ(driver.information(), QString(kChooserInformation));
    ASSERT_EQ(driver.buttonLabels(), QStringList({"Dump", "Read Param", "Relearn", "Set Param"}));
    ASSERT_EQ(static_cast<int>(action), expected_action);
}

TEST_F(DensoTcuReadPreflightTest, dismissingChooserReturnsCancelled)
{
    ChooserDriver driver{{}};
    driver.start();

    ASSERT_EQ(choose_denso_tcu_read_action(nullptr), DensoTcuReadAction::Cancelled);
    ASSERT_TRUE(driver.sawChooser());
    ASSERT_TRUE(!driver.timedOut());
}

struct dumpAndCancelledReturnWithoutIgnitionOrSerialCallsCase
{
    std::string name;
    int action;
    bool handled;
};
class dumpAndCancelledReturnWithoutIgnitionOrSerialCallsParameters
    : public DensoTcuReadPreflightTest,
      public ::testing::WithParamInterface<dumpAndCancelledReturnWithoutIgnitionOrSerialCallsCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, dumpAndCancelledReturnWithoutIgnitionOrSerialCallsParameters,
    ::testing::Values(dumpAndCancelledReturnWithoutIgnitionOrSerialCallsCase{"dump",
                                                                             static_cast<int>(DensoTcuReadAction::Dump),
                                                                             false},
                      dumpAndCancelledReturnWithoutIgnitionOrSerialCallsCase{
                          "cancelled", static_cast<int>(DensoTcuReadAction::Cancelled), true}),
    [](const ::testing::TestParamInfo<dumpAndCancelledReturnWithoutIgnitionOrSerialCallsCase>& info)
    { return info.param.name; });

TEST_P(dumpAndCancelledReturnWithoutIgnitionOrSerialCallsParameters, dumpAndCancelledReturnWithoutIgnitionOrSerialCalls)
{
    const int action = GetParam().action;
    const bool handled = GetParam().handled;
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.fake();
    ASSERT_TRUE(fake != nullptr);
    SerialPortActions& serial = adapter.connection().facade();
    expectNoBackendIo(*fake);

    ServiceActionDriver driver{false};
    driver.start();
    ASSERT_EQ(run_denso_tcu_service_action(static_cast<DensoTcuReadAction>(action), &serial, "sub_tcu_denso_sh7058_can",
                                           nullptr),
              handled);
    fastecu::testing::process_events_for(std::chrono::milliseconds(20));

    ASSERT_EQ(driver.ignitionCount(), 0);
    ASSERT_TRUE(driver.serviceDialogTitles().isEmpty());
}

struct decliningIgnitionSkipsEveryServiceDialogAndSerialCallCase
{
    std::string name;
    int action;
};
class decliningIgnitionSkipsEveryServiceDialogAndSerialCallParameters
    : public DensoTcuReadPreflightTest,
      public ::testing::WithParamInterface<decliningIgnitionSkipsEveryServiceDialogAndSerialCallCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, decliningIgnitionSkipsEveryServiceDialogAndSerialCallParameters,
    ::testing::Values(
        decliningIgnitionSkipsEveryServiceDialogAndSerialCallCase{"relearn",
                                                                  static_cast<int>(DensoTcuReadAction::Relearn)},
        decliningIgnitionSkipsEveryServiceDialogAndSerialCallCase{"read",
                                                                  static_cast<int>(DensoTcuReadAction::ReadParameters)},
        decliningIgnitionSkipsEveryServiceDialogAndSerialCallCase{"set",
                                                                  static_cast<int>(DensoTcuReadAction::SetParameters)}),
    [](const ::testing::TestParamInfo<decliningIgnitionSkipsEveryServiceDialogAndSerialCallCase>& info)
    { return info.param.name; });

TEST_P(decliningIgnitionSkipsEveryServiceDialogAndSerialCallParameters,
       decliningIgnitionSkipsEveryServiceDialogAndSerialCall)
{
    const int action = GetParam().action;
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.fake();
    ASSERT_TRUE(fake != nullptr);
    SerialPortActions& serial = adapter.connection().facade();
    expectNoBackendIo(*fake);

    ServiceActionDriver driver{false};
    driver.start();
    ASSERT_TRUE(run_denso_tcu_service_action(static_cast<DensoTcuReadAction>(action), &serial,
                                             "sub_tcu_denso_sh7058_can", nullptr));

    ASSERT_TRUE(!driver.timedOut());
    ASSERT_EQ(driver.ignitionCount(), 1);
    ASSERT_EQ(driver.ignitionIcon(), QMessageBox::Warning);
    ASSERT_EQ(driver.ignitionText(), QString(kIgnitionText));
    ASSERT_EQ(driver.ignitionButtons(), QMessageBox::Ok | QMessageBox::Cancel);
    ASSERT_TRUE(driver.serviceDialogTitles().isEmpty());
}

struct acceptingIgnitionOpensTheMatchingRealServiceDialogCase
{
    std::string name;
    int action;
    QString title;
};
class acceptingIgnitionOpensTheMatchingRealServiceDialogParameters
    : public DensoTcuReadPreflightTest,
      public ::testing::WithParamInterface<acceptingIgnitionOpensTheMatchingRealServiceDialogCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, acceptingIgnitionOpensTheMatchingRealServiceDialogParameters,
    ::testing::Values(
        acceptingIgnitionOpensTheMatchingRealServiceDialogCase{"relearn", static_cast<int>(DensoTcuReadAction::Relearn),
                                                               "TCU Relearn"},
        acceptingIgnitionOpensTheMatchingRealServiceDialogCase{
            "read", static_cast<int>(DensoTcuReadAction::ReadParameters), "Read TCU Parameters"},
        acceptingIgnitionOpensTheMatchingRealServiceDialogCase{
            "set", static_cast<int>(DensoTcuReadAction::SetParameters), "Set TCU Parameters"}),
    [](const ::testing::TestParamInfo<acceptingIgnitionOpensTheMatchingRealServiceDialogCase>& info)
    { return info.param.name; });

TEST_P(acceptingIgnitionOpensTheMatchingRealServiceDialogParameters, acceptingIgnitionOpensTheMatchingRealServiceDialog)
{
    const int action = GetParam().action;
    const QString title = GetParam().title;
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.fake();
    ASSERT_TRUE(fake != nullptr);
    SerialPortActions& serial = adapter.connection().facade();
    expectNoBackendIo(*fake);

    ServiceActionDriver driver{true};
    driver.start();
    ASSERT_TRUE(run_denso_tcu_service_action(static_cast<DensoTcuReadAction>(action), &serial,
                                             "sub_tcu_denso_sh7058_can", nullptr));

    ASSERT_TRUE(!driver.timedOut());
    ASSERT_EQ(driver.ignitionCount(), 1);
    ASSERT_EQ(driver.ignitionIcon(), QMessageBox::Warning);
    ASSERT_EQ(driver.ignitionText(), QString(kIgnitionText));
    ASSERT_EQ(driver.ignitionButtons(), QMessageBox::Ok | QMessageBox::Cancel);
    ASSERT_EQ(driver.serviceDialogTitles(), QStringList({title}));
}

} // namespace fastecu::service_functions

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment);
}
