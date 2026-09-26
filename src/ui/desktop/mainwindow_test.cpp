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
#include <QSignalSpy>
#include <QTimer>

#include <gmock/gmock.h>
#include "src/backend/logging/testing/scripted_logging_protocol.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <memory>
#include <utility>

#define private public
#include "src/ui/desktop/mainwindow.h"
#include "ui_mainwindow.h"
#undef private

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/logging/logging_engine.h"
#include "src/platform/desktop/common/logging/systemlogger.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/platform/desktop/common/remote_utility/remote_utility.h"

namespace
{

constexpr auto kTcuChooserText = "Choose which option";
constexpr auto kTcuIgnitionText = "Turn ignition ON and press OK to start initializing connection to TCU";
constexpr auto kLegacyEcuIgnitionText = "Turn ignition ON and press OK to start initializing connection to ECU";
constexpr auto kNoChecksumModuleText = "WARNING! There is no checksum module for this ROM!";
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

    int checksumWarningCount() const
    {
        return checksum_warning_count_;
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
                if (message_box->text().startsWith(kNoChecksumModuleText))
                {
                    ++checksum_warning_count_;
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
    int checksum_warning_count_ = 0;
    bool timed_out_ = false;
};

std::unique_ptr<SerialPortActions> fakeSerial(QObject *parent, FakeBackend **fake)
{
    auto serial = std::make_unique<SerialPortActions>(
        [fake]() -> SerialBackend *
        {
            *fake = new NiceFakeBackend;
            return *fake;
        },
        parent);
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

// The services DesktopComposition builds in the real app, minus the syslog
// thread: the logger lives on the test thread, which is enough for a receiver.
struct TestServices
{
    explicit TestServices(const QString& config_root)
        : file_actions(file_system, resource_bundle, file_repository, file_writer, events)
    {
        FileActions::ConfigValuesStructure *config = &file_actions.ConfigValuesStruct;
        file_actions.set_base_dirs(config, config_root.toStdString());
        syslogger = std::make_unique<SystemLogger>(config->syslog_files_directory, config->software_name,
                                                   config->software_version);
        serial = fakeSerial(nullptr, &fake);
    }

    MainWindowServices services()
    {
        return {
            .file_actions = file_actions,
            .config_repository = file_repository,
            .file_action_events = events,
            .syslogger = *syslogger,
            .serial = *serial,
            .remote_utility = remote_utility,
            .logging_engine = logging_engine,
        };
    }

    QtFileSystem file_system;
    QtResourceBundle resource_bundle;
    QtFileRepository file_repository;
    QtAtomicFileWriter file_writer;
    QtEventSink events;
    FileActions file_actions;
    std::unique_ptr<SystemLogger> syslogger;
    FakeBackend *fake = nullptr;
    std::unique_ptr<SerialPortActions> serial; // null if the fake backend failed to start
    RemoteUtility remote_utility{"", ""};
    fastecu::desktop::logging::LoggingEngine logging_engine;
};

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
    <protocol name="sub_ecu_denso_sh7058">
      <ecu>Denso SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>yes</checksum>
      <read>yes</read>
      <test_write>no</test_write>
      <write>yes</write>
      <flash_transport>K-Line</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0x2004</cal_id_addr>
      <cal_id_length>8</cal_id_length>
      <kernel>test-kernel.bin</kernel>
      <kernel_addr>0xFFFF3000</kernel_addr>
      <description>Denso SH7058 K-Line</description>
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
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        const QString version_dir = config_root_.path() + "/" + window.software_version + "/";
        QCOMPARE(window.configValues->base_config_directory, config_root_.path());
        QCOMPARE(window.configValues->config_file, version_dir + "config/fastecu.cfg");
        QCOMPARE(window.configValues->flash_protocol_model, QStringList{"Test"});
        QCOMPARE(window.configValues->syslog_files_directory, version_dir + "syslogs/");
        QVERIFY(QDir(version_dir + "syslogs").exists());
        QVERIFY(QDir(version_dir + "definitions").exists());
        QVERIFY(QFile::exists(window.configValues->config_file));
    }

    // A direct session (no peer address) must never wait for a remote source.
    void directSessionStartupNeverWaitsForARemoteSource()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        EXPECT_CALL(*services.fake, waitForSource()).Times(0);
        MainWindow window{services.services()};
        constructor_driver.stop();
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
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        FakeBackend *fake = services.fake;
        EXPECT_CALL(*fake, open_serial_port()).Times(0);
        EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
        // Characterization: the call counts observed on master (before step
        // 6d) for both rows, pinned so the dispatch refactor cannot change them.
        EXPECT_CALL(*fake, reset_connection()).Times(3);
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(2);

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

        // The TCU log lines must come from MainWindow, which outlives the
        // queued delivery to the syslogger's thread; a short-lived sender's
        // queued lines are dropped once it is destroyed.
        QSignalSpy info_lines{&window, &MainWindow::LOG_I};
        ModalDriver operation_driver{choice};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "read"), 0);

        QVERIFY(operation_driver.sawChooser());
        QCOMPARE(operation_driver.ignitionCount(), expected_ignition_count);
        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(operation_driver.unexpectedFlashDialogCount(), 0);

        QTest::qWait(window.vbatt_timer_timeout + 100);
        QVERIFY(!window.vbatt_timer->isActive());
        QVERIFY(window.ecuCalDef[window.ecuCalDefIndex] == nullptr);
        const QString expected_line = choice.isEmpty() ? "No option selected" : "Attempting TCU relearn";
        QVERIFY(std::ranges::any_of(info_lines, [&](const QList<QVariant>& arguments)
                                    { return arguments.at(0).toString() == expected_line; }));
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
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        FakeBackend *fake = services.fake;
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

    void representativePortableRoutesReachFactoryBeforeLegacyFallback_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QString>("mcu");
        QTest::addColumn<QString>("kernel_address");
        QTest::newRow("petrol") << QString("sub_ecu_denso_sh7058_can") << QString("SH7058") << QString("0xFFFF3000");
        QTest::newRow("densocan") << QString("sub_ecu_denso_sh7058_densocan") << QString("SH7058")
                                  << QString("0xFFFF3000");
        // Wave 6b-2: the Denso SH705x K-Line family (sub_ecu_denso_sh7055_04*
        // and sub_ecu_denso_sh7058*) moved off FlashEcuSubaruDensoSH705xKline
        // onto this same portable factory path; see
        // exactDensoKlineIdsStillDispatchToTheLegacyKlineDialog in prior
        // revisions of this file for the characterization test this replaces.
        QTest::newRow("denso_sh705x_kline")
            << QString("sub_ecu_denso_sh7058") << QString("SH7058") << QString("0xFFFF3000");
    }

    void representativePortableRoutesReachFactoryBeforeLegacyFallback()
    {
        QFETCH(QString, protocol);
        QFETCH(QString, mcu);
        QFETCH(QString, kernel_address);
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        FakeBackend *fake = services.fake;
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

    // Spec behavior change 1: "No file selected!" returns after the entry
    // reset started battery polling; it must now run the cleanup.
    void writeWithoutASelectedCalibrationStopsVoltagePolling()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        FakeBackend *fake = services.fake;
        EXPECT_CALL(*fake, open_serial_port()).Times(0);
        EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        window.configValues->flash_protocol_selected_make = "Subaru";
        window.configValues->flash_protocol_selected_protocol_name = "sub_ecu_denso_sh7058_can";
        window.configValues->flash_protocol_selected_mcu = "SH7058";
        window.configValues->flash_protocol_selected_id = "0";
        window.configValues->flash_protocol_kernel = {"test-kernel.bin"};
        window.configValues->flash_protocol_kernel_addr = {"0xFFFF3000"};
        window.configValues->kernel_files_directory = config_root_.path() + "/kernels/";

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "write"), 0);
        operation_driver.stop();

        QVERIFY(!operation_driver.timedOut());
        QVERIFY(!window.vbatt_timer->isActive());
    }

    // Characterization: only Subaru and Mitsubishi dispatch, but every make
    // gets the cleanup.
    void otherMakesSkipDispatchButStillRunCleanup()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        FakeBackend *fake = services.fake;
        EXPECT_CALL(*fake, open_serial_port()).Times(0);
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        window.configValues->flash_protocol_selected_make = "Nissan";
        window.configValues->kernel_files_directory = config_root_.path() + "/kernels/";

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "read"), 0);
        operation_driver.stop();

        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(operation_driver.unexpectedFlashDialogCount(), 0);
        QVERIFY(!window.vbatt_timer->isActive());
    }

    // Spec behavior change 2: a read that produces no calibration releases
    // the slot it allocated instead of leaking it.
    void readOfAnUnsupportedProtocolReleasesTheReadSlot()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        window.configValues->flash_protocol_selected_make = "Subaru";
        window.configValues->flash_protocol_selected_protocol_name = "sub_ecu_not_a_real_protocol";
        window.configValues->flash_protocol_selected_mcu = "SH7058";
        window.configValues->flash_protocol_selected_id = "0";
        window.configValues->flash_protocol_kernel = {"test-kernel.bin"};
        window.configValues->flash_protocol_kernel_addr = {"0xFFFF3000"};
        window.configValues->kernel_files_directory = config_root_.path() + "/kernels/";
        const int slot = window.ecuCalDefIndex;

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "read"), 0);
        operation_driver.stop();

        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(window.ecuCalDefIndex, slot);
        QVERIFY(window.ecuCalDef[slot] == nullptr);
    }

    // Spec behavior change 1, second early return: Cancel on the
    // no-checksum-module warning must also stop battery polling.
    void cancellingTheChecksumWarningStopsVoltagePolling()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        FakeBackend *fake = services.fake;
        EXPECT_CALL(*fake, open_serial_port()).Times(0);
        EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        window.configValues->flash_protocol_selected_make = "Subaru";
        window.configValues->flash_protocol_selected_protocol_name = "sub_ecu_denso_sh7058_can";
        window.configValues->flash_protocol_selected_checksum = "n/a";
        window.configValues->kernel_files_directory = config_root_.path() + "/kernels/";

        // One loaded calibration, selected in the calibration files tree.
        auto calibration = std::make_unique<FileActions::EcuCalDefStructure>();
        calibration->FullRomData = QByteArray(16, '\x5a');
        window.ecuCalDef[0] = calibration.get();
        auto *item = new QTreeWidgetItem(QStringList{"test.bin"});
        window.ui->calibrationFilesTreeWidget->addTopLevelItem(item);
        item->setSelected(true);

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "write"), 0);
        operation_driver.stop();
        window.ecuCalDef[0] = nullptr;

        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(operation_driver.checksumWarningCount(), 1);
        QCOMPARE(calibration->FullRomData, QByteArray(16, '\x5a'));
        QVERIFY(!window.vbatt_timer->isActive());
    }

    void windowPreservesInjectedLoggingFactory()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        bool called = false;
        services.logging_engine.registerProtocol(
            "SSM",
            [&called](const fastecu::desktop::logging::DesktopLoggingSnapshot& snapshot)
            {
                called = !snapshot.target_is_ecu;
                auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                protocol->blockPollUntilCancelled();
                return protocol;
            });
        MainWindow window{services.services()};
        constructor_driver.stop();
        // This test checks ownership, not UI error dialogs. The next test
        // drives actual menu dispatch and checks the selected target.
        QObject::disconnect(&services.logging_engine, nullptr, &window, nullptr);
        auto session = fastecu::logging::make_logging_session(
            fastecu::logging::LoggingProtocolId::Ssm,
            {{.id = "rpm",
              .address = 0x10,
              .length = 1,
              .raw_assembly = fastecu::logging::RawAssembly::UnsignedIntegerDecimal,
              .from_byte_expression = "x",
              .unit = "rpm",
              .decimal_precision = 0}},
            {.poll_timeout = std::chrono::milliseconds{50},
             .car_silence_miss_threshold = 20,
             .reconnect_attempt_threshold = 100,
             .reconnect_retry_period = 20});
        QVERIFY(session);
        QVERIFY(services.logging_engine.start(
            {.protocolId = "SSM"}, {.session = std::move(*session), .response_offsets = {0}, .target_is_ecu = false}));
        services.logging_engine.stop();
        QVERIFY(called);
    }

    void loggingCapturesTargetForEachRun()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        window.vbatt_timer->stop();
        window.ecu_init_complete = true;
        window.configValues->flash_protocol_selected_log_protocol = "SSM";
        window.protocol = "SSM";
        auto *menu = window.ui->menubar->addMenu("Test logging");
        auto *action = menu->addAction("Logging");
        action->setCheckable(true);
        auto& values = *window.logValues;
        values = FileActions::LogValuesStructure{};
        values.log_value_id = {"rpm"};
        values.log_value_protocol = {"SSM"};
        values.log_value_name = {"rpm"};
        values.log_value_description = {"rpm"};
        values.log_value_ecu_byte_index = {"0"};
        values.log_value_ecu_bit = {"0"};
        values.log_value_target = {"ECU"};
        values.log_value_address = {"000010"};
        values.log_value_conversions = {{{"rpm", "x", "0", "0", "100", "1"}}};
        values.log_value_length = {"1"};
        values.log_value = {"0"};
        values.log_value_enabled = {"1"};
        values.lower_panel_log_value_id = {"rpm"};
        std::vector<bool> targets;
        services.logging_engine.registerProtocol(
            "SSM",
            [&targets](const fastecu::desktop::logging::DesktopLoggingSnapshot& snapshot)
            {
                targets.push_back(snapshot.target_is_ecu);
                auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                protocol->blockPollUntilCancelled();
                return protocol;
            });
        for (bool target : {true, false})
        {
            window.ecu_radio_button->setAutoExclusive(false);
            window.ecu_radio_button->setChecked(target);
            action->setChecked(true);
            QVERIFY(QMetaObject::invokeMethod(&window, "menu_action_triggered", Qt::DirectConnection,
                                              Q_ARG(QString, QStringLiteral("toggle_realtime"))));
            QVERIFY(window.activeLoggingSnapshot.has_value());
            QCOMPARE(window.activeLoggingSnapshot->target_is_ecu, target);
            services.logging_engine.stop();
        }
        QCOMPARE(targets, (std::vector<bool>{true, false}));
    }

  private:
    QTemporaryDir config_root_;
};

int main(int argc, char **argv)
{
    std::fprintf(stderr, "MainWindowTest: entered main\n");
    ::testing::InitGoogleMock(&argc, argv);
    QApplication app(argc, argv);
    std::fprintf(stderr, "MainWindowTest: QApplication initialized\n");
    MainWindowTest test;
    const int result = QTest::qExec(&test, argc, argv);
    std::fprintf(stderr, "MainWindowTest: qExec returned %d\n", result);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "mainwindow_test.moc"
