#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <gmock/gmock.h>

#include <cstring>
#include <memory>
#include <utility>

#define private public
#include "src/ui/desktop/mainwindow.h"
#undef private

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

namespace
{

constexpr auto kTcuChooserText = "Choose which option";
constexpr auto kTcuIgnitionText = "Turn ignition ON and press OK to start initializing connection to TCU";
constexpr auto kLegacyEcuIgnitionText = "Turn ignition ON and press OK to start initializing connection to ECU";
constexpr auto kPortableEcuIgnitionText = "Turn ignition ON and press OK to start initializing the ECU connection.";

class ModalDriver final : public QObject
{
    Q_OBJECT

  public:
    explicit ModalDriver(QString chooser_choice) : chooser_choice_(std::move(chooser_choice))
    {
        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, &ModalDriver::drive);
    }

    void start()
    {
        elapsed_.start();
        timer_.start();
    }

    void stop()
    {
        timer_.stop();
    }

    bool sawChooser() const
    {
        return saw_chooser_;
    }

    int ignitionCount() const
    {
        return ignition_count_;
    }

    int unexpectedFlashDialogCount() const
    {
        return unexpected_flash_dialog_count_;
    }

    int legacyEcuIgnitionCount() const
    {
        return legacy_ecu_ignition_count_;
    }

    int portableEcuIgnitionCount() const
    {
        return portable_ecu_ignition_count_;
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
            if (auto *message_box = qobject_cast<QMessageBox *>(widget); message_box != nullptr)
            {
                if (message_box->text() == kTcuChooserText)
                {
                    saw_chooser_ = true;
                    if (chooser_choice_.isEmpty())
                    {
                        message_box->reject();
                        return;
                    }
                    for (QAbstractButton *button : message_box->buttons())
                    {
                        if (button->text() == chooser_choice_)
                        {
                            button->click();
                            return;
                        }
                    }
                }
                if (message_box->text() == kTcuIgnitionText)
                {
                    ++ignition_count_;
                    message_box->done(QMessageBox::Cancel);
                    return;
                }
                if (message_box->text() == kLegacyEcuIgnitionText)
                {
                    ++legacy_ecu_ignition_count_;
                    message_box->done(QMessageBox::Cancel);
                    return;
                }
                if (message_box->text() == kPortableEcuIgnitionText)
                {
                    ++portable_ecu_ignition_count_;
                    message_box->done(QMessageBox::Cancel);
                    return;
                }

                message_box->accept();
                return;
            }
            if (widget->inherits("fastecu::flash::FlashDialog"))
            {
                unexpected_flash_dialog_count_ = 1;
            }
        }

        if (elapsed_.elapsed() > 3000)
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
    QString chooser_choice_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    bool saw_chooser_ = false;
    int ignition_count_ = 0;
    int unexpected_flash_dialog_count_ = 0;
    int legacy_ecu_ignition_count_ = 0;
    int portable_ecu_ignition_count_ = 0;
    bool timed_out_ = false;
};

std::unique_ptr<SerialPortActions> fakeSerial(QObject *parent, FakeBackend **fake)
{
    auto serial = std::make_unique<SerialPortActions>("", "", nullptr, parent,
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

// start_ecu_operations is a private slot, and this file reaches MainWindow's
// internals through `#define private public` above. That works for data
// members, whose access is checked only while compiling, but not for a call to
// an out-of-line member function on MSVC: the Microsoft ABI encodes the access
// level in the mangled name, so a call compiled here as public looks for a
// symbol mainwindow.cpp never emitted and the Windows link fails on it alone.
// The Itanium ABI does not encode access, which is why Linux and macOS link
// either way. Calling the slot by name through the metaobject mangles nothing,
// and is what logging_engine_test.cpp already does for its private slots.
int startEcuOperations(MainWindow& window, const QString& cmd_type)
{
    int result = -1;
    if (!QMetaObject::invokeMethod(&window, "start_ecu_operations", Qt::DirectConnection, Q_RETURN_ARG(int, result),
                                   Q_ARG(QString, cmd_type)))
    {
        return -1;
    }
    return result;
}

bool writeTextFile(const QString& path, const char *contents)
{
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    return file.write(contents) == static_cast<qint64>(std::strlen(contents));
}

} // namespace

class MainWindowTest : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        QVERIFY(config_root_.isValid());
        // Pass the fixture root explicitly: Qt resolves the Windows home from
        // the account profile before trying HOME/USERPROFILE fallbacks.
        const QString config_dir =
            config_root_.path() + "/" + FileActions::ConfigValuesStructure{}.software_version + "/config/";
        qInfo() << "Fixture config:" << config_dir << "Qt home:" << QDir::homePath();
        QVERIFY(QDir().mkpath(config_dir));
        QVERIFY(writeTextFile(config_dir + "fastecu.cfg",
                              R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <software_settings>
    <setting name="window_size">
      <value width="maximized"/>
      <value height="maximized"/>
    </setting>
    <setting name="toolbar_iconsize"><value data="32"/></setting>
    <setting name="serial_port"><value data="OpenPort 2.0"/></setting>
    <setting name="protocol_id"><value data="0"/></setting>
    <setting name="flash_transport"><value data="iso15765"/></setting>
    <setting name="log_transport"><value data="K-Line"/></setting>
    <setting name="log_protocol"><value data="SSM"/></setting>
    <setting name="primary_definition_base"><value data="romraider"/></setting>
    <setting name="calibration_files"/>
    <setting name="calibration_files_directory"><value data="calibrations/"/></setting>
    <setting name="use_romraider_definitions"><value data="disabled"/></setting>
    <setting name="romraider_definition_files"><value data=""/></setting>
    <setting name="use_ecuflash_definitions"><value data="disabled"/></setting>
    <setting name="ecuflash_definition_files_directory"><value data=""/></setting>
    <setting name="logger_definition_file"><value data="logger.cfg"/></setting>
    <setting name="datalog_files_directory"><value data="datalogs/"/></setting>
  </software_settings>
</config>
)"));
        QVERIFY(writeTextFile(config_dir + "menu.cfg",
                              R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <ecu_menu_definitions/>
  <popup_menu_definitions/>
</config>
)"));
        QVERIFY(writeTextFile(config_dir + "logger.cfg",
                              R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <logger/>
</config>
)"));
        QVERIFY(writeTextFile(config_dir + "protocols.cfg",
                              R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <protocols>
    <protocol name="sub_tcu_denso_sh7058_can">
      <ecu>Denso TCU SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>n/a</checksum>
      <read>yes</read>
      <test_write>no</test_write>
      <write>yes</write>
      <flash_transport>iso15765,CAN</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <ecu_id_ascii>no</ecu_id_ascii>
      <ecu_id_addr/>
      <ecu_id_length/>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0</cal_id_addr>
      <cal_id_length>10</cal_id_length>
      <kernel>tcu_kernel.bin</kernel>
      <kernel_addr>0x100000</kernel_addr>
      <description>Denso TCU SH7058</description>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_can">
      <ecu>Denso SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>yes</checksum>
      <read>yes</read>
      <test_write>yes</test_write>
      <write>yes</write>
      <flash_transport>iso15765,CAN</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0x2004</cal_id_addr>
      <cal_id_length>8</cal_id_length>
      <kernel>test-kernel.bin</kernel>
      <kernel_addr>0xFFFF3000</kernel_addr>
      <description>Denso SH7058 CAN</description>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_densocan">
      <ecu>Denso SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>yes</checksum>
      <read>yes</read>
      <test_write>yes</test_write>
      <write>yes</write>
      <flash_transport>CAN</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0x2000</cal_id_addr>
      <cal_id_length>8</cal_id_length>
      <kernel>test-kernel.bin</kernel>
      <kernel_addr>0xFFFF3000</kernel_addr>
      <description>Denso SH7058 DensoCAN</description>
    </protocol>
  </protocols>
  <car_models>
    <car_model>
      <make>Subaru</make>
      <model>Test</model>
      <version>Test</version>
      <type>TCU</type>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_tcu_denso_sh7058_can</protocol>
    </car_model>
  </car_models>
</config>
)"));
        const QString kernel_dir = config_root_.path() + "/kernels/";
        QVERIFY(QDir().mkpath(kernel_dir));
        QVERIFY(writeTextFile(kernel_dir + "test-kernel.bin", "ABCD"));
    }

    void explicitConfigRootLoadsFixtureAndProvisionsDirectories()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        MainWindow window{"", "", nullptr, config_root_.path()};
        constructor_driver.stop();

        const QString version_dir = config_root_.path() + "/" + window.software_version + "/";
        QCOMPARE(window.configValues->base_config_directory, config_root_.path());
        QCOMPARE(window.configValues->config_file, version_dir + "config/fastecu.cfg");
        QCOMPARE(window.configValues->flash_protocol_model, QStringList{"Test"});
        QCOMPARE(window.configValues->syslog_files_directory, version_dir + "syslogs/");
        QVERIFY(QDir(version_dir + "syslogs").exists());
        QVERIFY(QDir(version_dir + "definitions").exists());
        QVERIFY(QFile::exists(window.configValues->config_file));
        QVERIFY(!constructor_driver.timedOut());
    }

    void handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling_data()
    {
        QTest::addColumn<QString>("choice");
        QTest::addColumn<int>("expected_ignition_count");
        QTest::newRow("chooser-cancelled") << QString() << 0;
        QTest::newRow("relearn-declined") << QString("Relearn") << 1;
    }

    void handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling()
    {
        QFETCH(QString, choice);
        QFETCH(int, expected_ignition_count);

        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        MainWindow window{"", "", nullptr, config_root_.path()};
        constructor_driver.stop();

        FakeBackend *fake = nullptr;
        std::unique_ptr<SerialPortActions> serial = fakeSerial(&window, &fake);
        QVERIFY(serial != nullptr);
        delete window.serial;
        window.serial = serial.release();
        EXPECT_CALL(*fake, set_use_openport2_adapter(true)).WillOnce(::testing::DoDefault());
        EXPECT_CALL(*fake, read_vbatt()).WillOnce(::testing::Return(12500UL));
        EXPECT_CALL(*fake, open_serial_port()).Times(0);
        EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
        if (choice.isEmpty())
        {
            EXPECT_CALL(*fake, reset_connection()).Times(0);
            EXPECT_CALL(*fake, change_port_speed(::testing::_)).Times(0);
        }
        else
        {
            ::testing::InSequence sequence;
            EXPECT_CALL(*fake, reset_connection()).WillOnce(::testing::Return());
            EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).WillOnce(::testing::Return(STATUS_SUCCESS));
        }

        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        window.configValues->flash_protocol_selected_make = "Subaru";
        window.configValues->flash_protocol_selected_protocol_name = "sub_tcu_denso_sh7058_can";
        window.configValues->flash_protocol_selected_mcu = "SH7058";
        window.configValues->flash_protocol_selected_id = "0";
        window.configValues->flash_protocol_kernel = {"tcu_kernel.bin"};
        window.configValues->flash_protocol_kernel_addr = {"0x100000"};
        window.configValues->kernel_files_directory = config_root_.path() + "/kernels/";

        ModalDriver operation_driver{choice};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "read"), 0);

        QVERIFY(operation_driver.sawChooser());
        QCOMPARE(operation_driver.ignitionCount(), expected_ignition_count);
        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(operation_driver.unexpectedFlashDialogCount(), 0);

        QTest::qWait(window.vbatt_timer_timeout + 100);
        QVERIFY(!window.vbatt_timer->isActive());
    }

    void futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::newRow("future-can") << QString("sub_ecu_denso_sh7058_can_future");
        QTest::newRow("extra-densocan") << QString("sub_ecu_denso_sh7058_densocan_extra");
    }

    void futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo()
    {
        QFETCH(QString, protocol);
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        MainWindow window{"", "", nullptr, config_root_.path()};
        constructor_driver.stop();

        FakeBackend *fake = nullptr;
        std::unique_ptr<SerialPortActions> serial = fakeSerial(&window, &fake);
        QVERIFY(serial != nullptr);
        delete window.serial;
        window.serial = serial.release();
        EXPECT_CALL(*fake, open_serial_port()).Times(0);
        EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        window.configValues->flash_protocol_selected_make = "Subaru";
        window.configValues->flash_protocol_selected_protocol_name = protocol;
        window.configValues->flash_protocol_selected_mcu = "SH7058";
        window.configValues->flash_protocol_selected_id = "0";
        window.configValues->flash_protocol_kernel = {"test-kernel.bin"};
        window.configValues->flash_protocol_kernel_addr = {"0xFFFF3000"};
        window.configValues->kernel_files_directory = config_root_.path() + "/kernels/";

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "read"), 0);
        operation_driver.stop();

        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(operation_driver.legacyEcuIgnitionCount(), 0);
        QCOMPARE(operation_driver.portableEcuIgnitionCount(), 0);
        QCOMPARE(operation_driver.unexpectedFlashDialogCount(), 0);
    }

    void exactDensoKlineIdsStillDispatchToTheLegacyKlineDialog_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::newRow("stock") << QString("sub_ecu_denso_sh7058");
        QTest::newRow("ecutek") << QString("sub_ecu_denso_sh7058_ecutek");
        QTest::newRow("cobb") << QString("sub_ecu_denso_sh7058_cobb");
    }

    void exactDensoKlineIdsStillDispatchToTheLegacyKlineDialog()
    {
        QFETCH(QString, protocol);
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        MainWindow window{"", "", nullptr, config_root_.path()};
        constructor_driver.stop();

        FakeBackend *fake = nullptr;
        std::unique_ptr<SerialPortActions> serial = fakeSerial(&window, &fake);
        QVERIFY(serial != nullptr);
        delete window.serial;
        window.serial = serial.release();
        EXPECT_CALL(*fake, open_serial_port()).Times(0);
        EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        window.configValues->flash_protocol_selected_make = "Subaru";
        window.configValues->flash_protocol_selected_protocol_name = protocol;
        window.configValues->flash_protocol_selected_mcu = "SH7058";
        window.configValues->flash_protocol_selected_id = "0";
        window.configValues->flash_protocol_kernel = {"test-kernel.bin"};
        window.configValues->flash_protocol_kernel_addr = {"0xFFFF3000"};
        window.configValues->kernel_files_directory = config_root_.path() + "/kernels/";

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "read"), 0);
        operation_driver.stop();

        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(operation_driver.legacyEcuIgnitionCount(), 1);
        QCOMPARE(operation_driver.portableEcuIgnitionCount(), 0);
        QCOMPARE(operation_driver.unexpectedFlashDialogCount(), 0);
    }

    void representativePortableRoutesReachFactoryBeforeLegacyFallback_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QString>("mcu");
        QTest::addColumn<QString>("kernel_address");
        QTest::newRow("petrol") << QString("sub_ecu_denso_sh7058_can") << QString("SH7058") << QString("0xFFFF3000");
        QTest::newRow("densocan") << QString("sub_ecu_denso_sh7058_densocan") << QString("SH7058")
                                  << QString("0xFFFF3000");
    }

    void representativePortableRoutesReachFactoryBeforeLegacyFallback()
    {
        QFETCH(QString, protocol);
        QFETCH(QString, mcu);
        QFETCH(QString, kernel_address);
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        MainWindow window{"", "", nullptr, config_root_.path()};
        constructor_driver.stop();

        FakeBackend *fake = nullptr;
        std::unique_ptr<SerialPortActions> serial = fakeSerial(&window, &fake);
        QVERIFY(serial != nullptr);
        delete window.serial;
        window.serial = serial.release();
        EXPECT_CALL(*fake, open_serial_port()).Times(0);
        EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        window.configValues->flash_protocol_selected_make = "Subaru";
        window.configValues->flash_protocol_selected_protocol_name = protocol;
        window.configValues->flash_protocol_selected_mcu = mcu;
        window.configValues->flash_protocol_selected_id = "0";
        window.configValues->flash_protocol_kernel = {"test-kernel.bin"};
        window.configValues->flash_protocol_kernel_addr = {kernel_address};
        window.configValues->kernel_files_directory = config_root_.path() + "/kernels/";

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "read"), 0);
        operation_driver.stop();

        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(operation_driver.legacyEcuIgnitionCount(), 0);
        QCOMPARE(operation_driver.portableEcuIgnitionCount(), 1);
    }

  private:
    QTemporaryDir config_root_;
};

QTEST_MAIN(MainWindowTest)
#include "mainwindow_test.moc"
