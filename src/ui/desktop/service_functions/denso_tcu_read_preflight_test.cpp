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
#include "src/ui/desktop/service_functions/dialog/service_function_dialog.h"

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
            auto *messageBox = qobject_cast<QMessageBox *>(widget);
            if (messageBox == nullptr || messageBox->text() != kChooserText)
            {
                continue;
            }

            saw_chooser_ = true;
            text_ = messageBox->text();
            information_ = messageBox->informativeText();
            for (QAbstractButton *button : messageBox->buttons())
            {
                button_labels_.push_back(button->text());
            }
            button_labels_.sort();

            if (choice_.isEmpty())
            {
                messageBox->reject();
                return;
            }
            for (QAbstractButton *button : messageBox->buttons())
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
    explicit ServiceActionDriver(bool acceptIgnition) : accept_ignition_(acceptIgnition)
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
            if (auto *messageBox = qobject_cast<QMessageBox *>(widget);
                messageBox != nullptr && messageBox->text() == kIgnitionText)
            {
                ++ignition_count_;
                ignition_icon_ = messageBox->icon();
                ignition_text_ = messageBox->text();
                ignition_buttons_ = messageBox->standardButtons();
                messageBox->done(accept_ignition_ ? QMessageBox::Ok : QMessageBox::Cancel);
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
    EXPECT_CALL(fake, IsSerialPortOpen()).Times(0);
    EXPECT_CALL(fake, ResetConnection()).Times(0);
    EXPECT_CALL(fake, ChangePortSpeed(::testing::_)).Times(0);
    EXPECT_CALL(fake, OpenSerialPort()).Times(0);
    EXPECT_CALL(fake, ReadSerialData(::testing::_)).Times(0);
    EXPECT_CALL(fake, WriteSerialData(::testing::_)).Times(0);
    EXPECT_CALL(fake, WriteSerialDataEchoCheck(::testing::_)).Times(0);
    EXPECT_CALL(fake, ReadVbatt()).Times(0);
}

} // namespace

struct ChooserReturnsTheActionNamedByEachLegacyButtonCase
{
    std::string name;
    QString choice;
    int expected_action;
};
class ChooserReturnsTheActionNamedByEachLegacyButtonParameters
    : public ::testing::Test,
      public ::testing::WithParamInterface<ChooserReturnsTheActionNamedByEachLegacyButtonCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, ChooserReturnsTheActionNamedByEachLegacyButtonParameters,
    ::testing::Values(ChooserReturnsTheActionNamedByEachLegacyButtonCase{"dump", "Dump",
                                                                         static_cast<int>(DensoTcuReadAction::kDump)},
                      ChooserReturnsTheActionNamedByEachLegacyButtonCase{
                          "relearn", "Relearn", static_cast<int>(DensoTcuReadAction::kRelearn)},
                      ChooserReturnsTheActionNamedByEachLegacyButtonCase{
                          "read", "Read Param", static_cast<int>(DensoTcuReadAction::kReadParameters)},
                      ChooserReturnsTheActionNamedByEachLegacyButtonCase{
                          "set", "Set Param", static_cast<int>(DensoTcuReadAction::kSetParameters)}),
    [](const ::testing::TestParamInfo<ChooserReturnsTheActionNamedByEachLegacyButtonCase>& info)
    { return info.param.name; });

TEST_P(ChooserReturnsTheActionNamedByEachLegacyButtonParameters, chooserReturnsTheActionNamedByEachLegacyButton)
{
    const QString choice = GetParam().choice;
    const int expectedAction = GetParam().expected_action;

    ChooserDriver driver{choice};
    driver.start();
    const DensoTcuReadAction action = choose_denso_tcu_read_action(nullptr);

    ASSERT_TRUE(driver.sawChooser());
    ASSERT_TRUE(!driver.timedOut());
    ASSERT_EQ(driver.text(), QString(kChooserText));
    ASSERT_EQ(driver.information(), QString(kChooserInformation));
    ASSERT_EQ(driver.buttonLabels(), QStringList({"Dump", "Read Param", "Relearn", "Set Param"}));
    ASSERT_EQ(static_cast<int>(action), expectedAction);
}

TEST(DensoTcuReadPreflightTest, dismissingChooserReturnsCancelled)
{
    ChooserDriver driver{{}};
    driver.start();

    ASSERT_EQ(choose_denso_tcu_read_action(nullptr), DensoTcuReadAction::kCancelled);
    ASSERT_TRUE(driver.sawChooser());
    ASSERT_TRUE(!driver.timedOut());
}

struct DumpAndCancelledReturnWithoutIgnitionOrSerialCallsCase
{
    std::string name;
    int action;
    bool handled;
};
class DumpAndCancelledReturnWithoutIgnitionOrSerialCallsParameters
    : public ::testing::Test,
      public ::testing::WithParamInterface<DumpAndCancelledReturnWithoutIgnitionOrSerialCallsCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, DumpAndCancelledReturnWithoutIgnitionOrSerialCallsParameters,
    ::testing::Values(
        DumpAndCancelledReturnWithoutIgnitionOrSerialCallsCase{"dump", static_cast<int>(DensoTcuReadAction::kDump),
                                                               false},
        DumpAndCancelledReturnWithoutIgnitionOrSerialCallsCase{"cancelled",
                                                               static_cast<int>(DensoTcuReadAction::kCancelled), true}),
    [](const ::testing::TestParamInfo<DumpAndCancelledReturnWithoutIgnitionOrSerialCallsCase>& info)
    { return info.param.name; });

TEST_P(DumpAndCancelledReturnWithoutIgnitionOrSerialCallsParameters, dumpAndCancelledReturnWithoutIgnitionOrSerialCalls)
{
    const int action = GetParam().action;
    const bool handled = GetParam().handled;
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.Fake();
    ASSERT_TRUE(fake != nullptr);
    SerialPortActions& serial = adapter.Connection().Facade();
    expectNoBackendIo(*fake);

    ServiceActionDriver driver{false};
    driver.start();
    ASSERT_EQ(run_denso_tcu_service_action(static_cast<DensoTcuReadAction>(action), &serial, "sub_tcu_denso_sh7058_can",
                                           nullptr),
              handled);
    fastecu::testing::ProcessEventsFor(std::chrono::milliseconds(20));

    ASSERT_EQ(driver.ignitionCount(), 0);
    ASSERT_TRUE(driver.serviceDialogTitles().isEmpty());
}

struct DecliningIgnitionSkipsEveryServiceDialogAndSerialCallCase
{
    std::string name;
    int action;
};
class DecliningIgnitionSkipsEveryServiceDialogAndSerialCallParameters
    : public ::testing::Test,
      public ::testing::WithParamInterface<DecliningIgnitionSkipsEveryServiceDialogAndSerialCallCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, DecliningIgnitionSkipsEveryServiceDialogAndSerialCallParameters,
    ::testing::Values(
        DecliningIgnitionSkipsEveryServiceDialogAndSerialCallCase{"relearn",
                                                                  static_cast<int>(DensoTcuReadAction::kRelearn)},
        DecliningIgnitionSkipsEveryServiceDialogAndSerialCallCase{
            "read", static_cast<int>(DensoTcuReadAction::kReadParameters)},
        DecliningIgnitionSkipsEveryServiceDialogAndSerialCallCase{
            "set", static_cast<int>(DensoTcuReadAction::kSetParameters)}),
    [](const ::testing::TestParamInfo<DecliningIgnitionSkipsEveryServiceDialogAndSerialCallCase>& info)
    { return info.param.name; });

TEST_P(DecliningIgnitionSkipsEveryServiceDialogAndSerialCallParameters,
       decliningIgnitionSkipsEveryServiceDialogAndSerialCall)
{
    const int action = GetParam().action;
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.Fake();
    ASSERT_TRUE(fake != nullptr);
    SerialPortActions& serial = adapter.Connection().Facade();
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

struct AcceptingIgnitionOpensTheMatchingRealServiceDialogCase
{
    std::string name;
    int action;
    QString title;
};
class AcceptingIgnitionOpensTheMatchingRealServiceDialogParameters
    : public ::testing::Test,
      public ::testing::WithParamInterface<AcceptingIgnitionOpensTheMatchingRealServiceDialogCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, AcceptingIgnitionOpensTheMatchingRealServiceDialogParameters,
    ::testing::Values(
        AcceptingIgnitionOpensTheMatchingRealServiceDialogCase{
            "relearn", static_cast<int>(DensoTcuReadAction::kRelearn), "TCU Relearn"},
        AcceptingIgnitionOpensTheMatchingRealServiceDialogCase{
            "read", static_cast<int>(DensoTcuReadAction::kReadParameters), "Read TCU Parameters"},
        AcceptingIgnitionOpensTheMatchingRealServiceDialogCase{
            "set", static_cast<int>(DensoTcuReadAction::kSetParameters), "Set TCU Parameters"}),
    [](const ::testing::TestParamInfo<AcceptingIgnitionOpensTheMatchingRealServiceDialogCase>& info)
    { return info.param.name; });

TEST_P(AcceptingIgnitionOpensTheMatchingRealServiceDialogParameters, acceptingIgnitionOpensTheMatchingRealServiceDialog)
{
    const int action = GetParam().action;
    const QString title = GetParam().title;
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.Fake();
    ASSERT_TRUE(fake != nullptr);
    SerialPortActions& serial = adapter.Connection().Facade();
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
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
