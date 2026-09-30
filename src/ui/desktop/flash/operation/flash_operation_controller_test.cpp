#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include "src/ui/desktop/flash/operation/flash_operation_controller.h"

#include <QApplication>
#include <QMessageBox>
#include <gtest/gtest.h>
#include <QTimer>

#include <gmock/gmock.h>

#include "src/platform/desktop/common/connection/testing/adapter_connection_harness.h"

namespace fastecu::flash
{
namespace
{

// Answers every message box: rejects the Denso TCU chooser, accepts anything
// else, and records the texts it saw.
class BoxDriver final : public QObject
{

  public:
    BoxDriver()
    {
        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, &BoxDriver::drive);
        timer_.start();
    }

    QStringList texts;

  private:
    void drive()
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *box = qobject_cast<QMessageBox *>(widget); box != nullptr && box->isVisible())
            {
                texts << box->text();
                if (box->text() == "Choose which option")
                {
                    box->reject();
                }
                else
                {
                    box->accept();
                }
                return;
            }
        }
    }

  private:
    QTimer timer_;
};

void expectNoEcuIo(FakeBackend& fake)
{
    EXPECT_CALL(fake, open_serial_port()).Times(0);
    EXPECT_CALL(fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(fake, read_serial_data(::testing::_)).Times(0);
}

} // namespace

TEST(FlashOperationControllerTest, unknownProtocolIsUnsupportedAndWarnsWithoutSerialIo)
{
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.fake();
    ASSERT_TRUE(fake != nullptr);
    SerialPortActions& serial = adapter.connection().facade();
    expectNoEcuIo(*fake);
    FlashOperationController controller{serial, nullptr};
    BoxDriver driver;

    const FlashOperationOutcome outcome = controller.run({
        .operation = FlashOperation::Read,
        .protocol = "sub_ecu_not_a_real_protocol",
        .mcu = "SH7058",
        .kernel_path = "/k/kernel.bin",
        .image = std::nullopt,
        .paths = {},
        .display_filename = "",
    });

    ASSERT_EQ(outcome.status, FlashOperationStatus::Unsupported);
    ASSERT_TRUE(!outcome.read_bytes.has_value());
    ASSERT_EQ(driver.texts,
              QStringList{"Unknown flashmethod! Flashmethod \"sub_ecu_not_a_real_protocol\" not yet implemented!"});
}

TEST(FlashOperationControllerTest, cancelledDensoTcuChooserIsHandledWithoutSerialIo)
{
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.fake();
    ASSERT_TRUE(fake != nullptr);
    SerialPortActions& serial = adapter.connection().facade();
    expectNoEcuIo(*fake);
    FlashOperationController controller{serial, nullptr};
    BoxDriver driver;

    const FlashOperationOutcome outcome = controller.run({
        .operation = FlashOperation::Read,
        .protocol = "sub_tcu_denso_sh7058_can",
        .mcu = "SH7058",
        .kernel_path = "/k/tcu_kernel.bin",
        .image = std::nullopt,
        .paths = {},
        .display_filename = "",
    });

    ASSERT_EQ(outcome.status, FlashOperationStatus::ServiceActionHandled);
    ASSERT_EQ(driver.texts, QStringList{"Choose which option"});
}

} // namespace fastecu::flash

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment);
}
