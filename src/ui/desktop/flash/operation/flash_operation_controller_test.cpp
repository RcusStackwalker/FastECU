#include "src/ui/desktop/flash/operation/flash_operation_controller.h"

#include <QApplication>
#include <QMessageBox>
#include <QTest>
#include <QTimer>

#include <gmock/gmock.h>

#include <memory>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

namespace fastecu::flash
{
namespace
{

// Answers every message box: rejects the Denso TCU chooser, accepts anything
// else, and records the texts it saw.
class BoxDriver final : public QObject
{
    Q_OBJECT

  public:
    BoxDriver()
    {
        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, &BoxDriver::drive);
        timer_.start();
    }

    QStringList texts;

  private slots:
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

std::unique_ptr<SerialPortActions> fakeSerial(FakeBackend **fake)
{
    auto serial = std::make_unique<SerialPortActions>(
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

void expectNoEcuIo(FakeBackend& fake)
{
    EXPECT_CALL(fake, open_serial_port()).Times(0);
    EXPECT_CALL(fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(fake, read_serial_data(::testing::_)).Times(0);
}

} // namespace

class FlashOperationControllerTest : public QObject
{
    Q_OBJECT

  private slots:
    void unknownProtocolIsUnsupportedAndWarnsWithoutSerialIo()
    {
        FakeBackend *fake = nullptr;
        std::unique_ptr<SerialPortActions> serial = fakeSerial(&fake);
        QVERIFY(serial != nullptr);
        expectNoEcuIo(*fake);
        FlashOperationController controller{*serial, nullptr};
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

        QCOMPARE(outcome.status, FlashOperationStatus::Unsupported);
        QVERIFY(!outcome.read_bytes.has_value());
        QCOMPARE(driver.texts,
                 QStringList{"Unknown flashmethod! Flashmethod \"sub_ecu_not_a_real_protocol\" not yet implemented!"});
    }

    void cancelledDensoTcuChooserIsHandledWithoutSerialIo()
    {
        FakeBackend *fake = nullptr;
        std::unique_ptr<SerialPortActions> serial = fakeSerial(&fake);
        QVERIFY(serial != nullptr);
        expectNoEcuIo(*fake);
        FlashOperationController controller{*serial, nullptr};
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

        QCOMPARE(outcome.status, FlashOperationStatus::ServiceActionHandled);
        QCOMPARE(driver.texts, QStringList{"Choose which option"});
    }
};

} // namespace fastecu::flash

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QApplication application(argc, argv);
    fastecu::flash::FlashOperationControllerTest test;
    const int result = QTest::qExec(&test, argc, argv);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "flash_operation_controller_test.moc"
