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
#include <QSemaphore>
#include <QTimer>

#include <gmock/gmock.h>
#include "src/backend/logging/testing/scripted_logging_protocol.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <memory>
#include <initializer_list>
#include <utility>

#define private public
#include "src/ui/desktop/mainwindow.h"
#include "ui_mainwindow.h"
#undef private

#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/connection/testing/adapter_connection_harness.h"
#include "src/platform/desktop/common/logging/logging_engine.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/ui/desktop/channels/log_channel.h"
#include "src/ui/desktop/channels/remote_peer.h"

namespace
{

const ApplicationIdentity kTestApplication{.name = "FastECU", .title = "FastECU", .version = "0.1.0-beta.5"};

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

QByteArray frame(std::initializer_list<int> values)
{
    QByteArray out;
    for (int v : values)
    {
        out.append(static_cast<char>(v));
    }
    return out;
}

// A valid SSM2 ECU init response carrying ECU ID 3152584006.
const QByteArray kEcuInit = frame({0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x6C});

// Selects the fixture's last vehicle row using `protocol`.
void selectProtocol(MainWindow& window, const QString& protocol)
{
    QVERIFY(window.configSession->select_by_protocol_name(protocol.toStdString()));
}

// Selects the fixture's first Subaru row using `protocol`. The flash tests
// dispatch only for Subaru/Mitsubishi, and sub_ecu_denso_sh7058's last row
// is a Nissan one.
void selectSubaruProtocol(MainWindow& window, const QString& protocol)
{
    const auto vehicles = window.configSession->vehicles();
    const auto it =
        std::ranges::find_if(vehicles, [&](const fastecu::config::ResolvedCarModel& vehicle)
                             { return vehicle.make == "Subaru" && vehicle.protocol_name == protocol.toStdString(); });
    QVERIFY(it != vehicles.end());
    QVERIFY(window.configSession->select_row(static_cast<std::size_t>(it - vehicles.begin())).has_value());
}

// Selects the fixture's first vehicle row of `make`.
void selectMake(MainWindow& window, const QString& make)
{
    const auto vehicles = window.configSession->vehicles();
    const auto it = std::ranges::find(vehicles, make.toStdString(), &fastecu::config::ResolvedCarModel::make);
    QVERIFY(it != vehicles.end());
    QVERIFY(window.configSession->select_row(static_cast<std::size_t>(it - vehicles.begin())).has_value());
}

// Points the window at one open port on the given make and log transport.
void prepareConnect(MainWindow& window, FakeBackend& fake, const QString& make, const QString& transport)
{
    window.vbatt_timer->stop();
    window.serial_ports = {"ttyUSB0"};
    window.serial_port_list->clear();
    window.serial_port_list->addItem("ttyUSB0");
    selectMake(window, make);
    window.configSession->settings().selected_log_transport = transport.toStdString();
    window.configSession->settings().selected_log_protocol = "SSM";
    ON_CALL(fake, open_serial_port()).WillByDefault(::testing::Return(QString("ttyUSB0")));
}

bool triggerMenu(MainWindow& window, const char *command)
{
    return QMetaObject::invokeMethod(&window, "menu_action_triggered", Qt::DirectConnection,
                                     Q_ARG(QString, QString::fromLatin1(command)));
}

// The services DesktopComposition builds in the real app. The channels are
// left unwired: tests spy on them.
struct TestServices
{
    explicit TestServices(const QString& config_root)
        : config_status(config.initialize(config_root.toStdString(), kTestApplication.version)),
          file_actions(file_system, resource_bundle, file_repository, file_writer, events, config)
    {
    }

    MainWindowServices services()
    {
        return {
            .application = kTestApplication,
            .config = config,
            .file_actions = file_actions,
            .config_repository = file_repository,
            .file_action_events = events,
            .log = log_channel,
            .connection = adapter.connection(),
            .remote = remote_peer,
            .logging_engine = logging_engine,
        };
    }

    QtFileSystem file_system;
    QtResourceBundle resource_bundle;
    QtFileRepository file_repository;
    QtAtomicFileWriter file_writer;
    QtEventSink events;
    QtEventSink config_events;
    fastecu::config::ConfigSession config{file_system, resource_bundle, file_repository, config_events};
    fastecu::Status config_status; // declared after `config`: initialized from it
    FileActions file_actions;
    fastecu::ui::LogChannel log_channel;
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.fake(); // null if the fake backend failed to start
    fastecu::ui::RemotePeer remote_peer;
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
            config_root_.path() + "/" + QString::fromStdString(kTestApplication.version) + "/config/";
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
    <protocol name="sub_ecu_denso_sh7058_can_future">
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
    <protocol name="sub_ecu_denso_sh7058_densocan_extra">
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
    <protocol name="sub_ecu_not_a_real_protocol">
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
    <protocol name="sub_ecu_denso_sh7058_can_checksum_na">
      <ecu>Denso SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>n/a</checksum>
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
    <car_model>
      <make>Subaru</make>
      <model>Can</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058_can</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>DensoCan</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058_densocan</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>KLine</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>Future</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058_can_future</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>Extra</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058_densocan_extra</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>Unsupported</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_not_a_real_protocol</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>ChecksumNa</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058_can_checksum_na</protocol>
    </car_model>
    <car_model>
      <make>Mitsubishi</make>
      <model>Colt</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058</protocol>
    </car_model>
    <car_model>
      <make>Nissan</make>
      <model>Test</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>Orphan</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_orphan</protocol>
    </car_model>
  </car_models>
</config>
)"));
        const QString kernel_dir =
            config_root_.path() + "/" + QString::fromStdString(kTestApplication.version) + "/kernels/";
        QVERIFY(QDir().mkpath(kernel_dir));
        QVERIFY(writeTextFile(kernel_dir + "test-kernel.bin", "ABCD"));
        QVERIFY(writeTextFile(kernel_dir + "tcu_kernel.bin", "ABCD"));
    }

    void explicitConfigRootLoadsFixtureAndProvisionsDirectories()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        const QString version_dir = config_root_.path() + "/" + window.software_version + "/";
        const fastecu::config::ConfigPaths paths = window.configSession->provisioned_paths();
        QCOMPARE(paths.base_config_directory, config_root_.path().toStdString());
        QCOMPARE(paths.config_file, (version_dir + "config/fastecu.cfg").toStdString());
        QCOMPARE(window.configSession->vehicles().front().model, std::string("Test"));
        QCOMPARE(paths.syslog_files_directory, (version_dir + "syslogs/").toStdString());
        QVERIFY(QDir(version_dir + "syslogs").exists());
        QVERIFY(QDir(version_dir + "definitions").exists());
        QVERIFY(QFile::exists(QString::fromStdString(paths.config_file)));
    }

    // A direct session (no peer address) must never wait for a remote source.
    void directSessionStartupNeverWaitsForARemoteSource()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        EXPECT_CALL(*services.fake, waitForSource()).Times(0);
        MainWindow window{services.services()};
        constructor_driver.stop();
    }

    void windowLogLinesReachTheLogChannel()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QSignalSpy lines{&services.log_channel, &fastecu::ui::LogChannel::LOG_I};

        emit window.LOG_I("probe line", true, false);

        QCOMPARE(lines.count(), 1);
        QCOMPARE(lines.at(0).at(0).toString(), QString("probe line"));
        QCOMPARE(lines.at(0).at(1).toBool(), true);
        QCOMPARE(lines.at(0).at(2).toBool(), false);
    }

    void windowEnablesFileLoggingThroughTheChannel()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QSignalSpy enables{&services.log_channel, &fastecu::ui::LogChannel::enable_log_write_to_file};
        MainWindow window{services.services()};
        constructor_driver.stop();

        QVERIFY(
            std::ranges::any_of(enables, [](const QList<QVariant>& arguments) { return arguments.at(0).toBool(); }));
    }

    void directSessionStartupNeverRequestsTheRemoteWait()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QSignalSpy waits{&services.remote_peer, &fastecu::ui::RemotePeer::wait_requested};
        MainWindow window{services.services()};
        constructor_driver.stop();

        QCOMPARE(waits.count(), 0);
    }

    void externalLoggerMirrorsToTheRemotePeer()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QSignalSpy lines{&services.remote_peer, &fastecu::ui::RemotePeer::log_window_message};
        QSignalSpy progress{&services.remote_peer, &fastecu::ui::RemotePeer::progress};

        // Private slots: call by name so the Windows link needs no mangled
        // private symbol (see startEcuOperations).
        QVERIFY(QMetaObject::invokeMethod(&window, "external_logger", Qt::DirectConnection,
                                          Q_ARG(QString, QString("mirrored line"))));
        QVERIFY(QMetaObject::invokeMethod(&window, "external_logger_set_progressbar_value", Qt::DirectConnection,
                                          Q_ARG(int, 42)));

        QCOMPARE(lines.count(), 1);
        QCOMPARE(lines.at(0).at(0).toString(), QString("mirrored line"));
        QCOMPARE(progress.count(), 1);
        QCOMPARE(progress.at(0).at(0).toInt(), 42);
    }

    void peerStateChangesReachTheWindow()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QSignalSpy debug_lines{&window, &MainWindow::LOG_D};

        emit services.remote_peer.stateChanged(QRemoteObjectReplica::Valid, QRemoteObjectReplica::Default);

        QVERIFY(std::ranges::any_of(debug_lines, [](const QList<QVariant>& arguments)
                                    { return arguments.at(0).toString() == "Network connection established"; }));
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
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
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
        selectSubaruProtocol(window, "sub_tcu_denso_sh7058_can");

        // The TCU log lines are relayed through MainWindow's own LOG_* signals.
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
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
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
        selectSubaruProtocol(window, protocol);

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
        QTest::newRow("petrol") << QString("sub_ecu_denso_sh7058_can");
        QTest::newRow("densocan") << QString("sub_ecu_denso_sh7058_densocan");
        // Wave 6b-2: the Denso SH705x K-Line family (sub_ecu_denso_sh7055_04*
        // and sub_ecu_denso_sh7058*) moved off FlashEcuSubaruDensoSH705xKline
        // onto this same portable factory path; see
        // exactDensoKlineIdsStillDispatchToTheLegacyKlineDialog in prior
        // revisions of this file for the characterization test this replaces.
        QTest::newRow("denso_sh705x_kline") << QString("sub_ecu_denso_sh7058");
    }

    void representativePortableRoutesReachFactoryBeforeLegacyFallback()
    {
        QFETCH(QString, protocol);
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
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
        selectSubaruProtocol(window, protocol);

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
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
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
        selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can");

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
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        FakeBackend *fake = services.fake;
        EXPECT_CALL(*fake, open_serial_port()).Times(0);
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        selectMake(window, "Nissan");

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
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        selectSubaruProtocol(window, "sub_ecu_not_a_real_protocol");
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
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
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
        selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can_checksum_na");

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
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
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
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        window.configSession->settings().selected_log_protocol = "SSM";
        QAction *action = prepareLogging(window, "SSM");
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

    void acceptedVehicleChoiceSelectsTheRowAndSavesIt()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        const std::string flash_transport = services.config.settings().selected_flash_transport;
        const std::string log_transport = services.config.settings().selected_log_transport;

        window.apply_vehicle_choice(QDialog::Accepted, 1);

        QCOMPARE(*services.config.selected_row(), std::size_t{1});
        QCOMPARE(services.config.settings().selected_log_protocol, std::string("SSM"));
        QCOMPARE(services.config.settings().selected_flash_transport, flash_transport);
        QCOMPARE(services.config.settings().selected_log_transport, log_transport);

        // Saved: a fresh session over the same root restores row 1.
        QtEventSink reread_events;
        fastecu::config::ConfigSession reread{services.file_system, services.resource_bundle, services.file_repository,
                                              reread_events};
        QVERIFY(reread.initialize(config_root_.path().toStdString(), kTestApplication.version).has_value());
        QCOMPARE(reread.settings().selected_protocol_id, std::string("1"));
    }

    void cancelledVehicleChoiceChangesNothing()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        const auto before = services.config.settings();

        window.apply_vehicle_choice(QDialog::Rejected, 1);

        QVERIFY(services.config.settings() == before);
    }

    void acceptedProtocolChoiceSelectsTheLastMatchingRow()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        const auto vehicles = services.config.vehicles();
        std::size_t last = 0;
        for (std::size_t i = 0; i < vehicles.size(); ++i)
        {
            if (vehicles[i].protocol_name == "sub_ecu_denso_sh7058")
            {
                last = i;
            }
        }

        window.apply_protocol_choice(QDialog::Accepted, std::string("sub_ecu_denso_sh7058"));

        QCOMPARE(*services.config.selected_row(), last);
    }

    void romFlashMethodSelectsTheLastMatchingRow()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        auto calibration = std::make_unique<FileActions::EcuCalDefStructure>();
        calibration->RomInfo.resize(FileActions::DefFile + 1);
        calibration->RomInfo[FileActions::FlashMethod] = "sub_ecu_denso_sh7058";
        window.ecuCalDef[0] = calibration.get();

        window.update_protocol_info(0);
        window.ecuCalDef[0] = nullptr;

        QCOMPARE(*services.config.selected_row(), std::size_t{9}); // rows 3, 8, 9 match; the last wins
        QCOMPARE(services.config.selected_vehicle()->make, std::string("Nissan"));
    }

    void unmatchedRomFlashMethodChangesNothing()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        const auto before = services.config.settings();
        auto calibration = std::make_unique<FileActions::EcuCalDefStructure>();
        calibration->RomInfo.resize(FileActions::DefFile + 1);
        calibration->RomInfo[FileActions::FlashMethod] = "no_such_protocol";
        window.ecuCalDef[0] = calibration.get();

        window.update_protocol_info(0);
        window.ecuCalDef[0] = nullptr;

        QVERIFY(services.config.settings() == before);
    }

    void unresolvedProtocolRowLeavesReadAndWriteUnavailable()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();

        // The fixture's menu.cfg is empty; add the three actions
        // set_flash_arrow_state looks up by text.
        auto *menu = window.ui->menubar->addMenu("Test flash");
        const QList<QAction *> actions{menu->addAction("Read from ecu"), menu->addAction("Test write to ecu"),
                                       menu->addAction("Write to ecu")};

        // A resolved row with every capability enables all three...
        selectProtocol(window, "sub_ecu_denso_sh7058_can");
        window.set_flash_arrow_state();
        for (QAction *action : actions)
        {
            QVERIFY2(action->isEnabled(), qPrintable(action->text()));
        }

        // ...and the unresolved row 10 (no <protocol> of that name) none.
        selectProtocol(window, "sub_ecu_orphan");
        window.set_flash_arrow_state();
        for (QAction *action : actions)
        {
            QVERIFY2(!action->isEnabled(), qPrintable(action->text()));
        }
    }

    void loggingUsesTheSessionLogProtocol()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareLogging(window, "CDBG");
        services.logging_engine.registerProtocol("CDBG",
                                                 [](const fastecu::desktop::logging::DesktopLoggingSnapshot&)
                                                 {
                                                     auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                     protocol->blockPollUntilCancelled();
                                                     return protocol;
                                                 });
        window.configSession->settings().selected_log_protocol = "CDBG";

        ModalDriver driver{QString()};
        driver.start();
        window.continue_start_logging();
        driver.stop();
        services.logging_engine.stop();

        QCOMPARE(window.activeLogValueProtocolFilter, QString("CDBG"));
    }

    void selectedSerialPortIsEmptyWithoutPorts()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        window.serial_ports.clear();
        window.serial_port_list->clear();
        QCOMPARE(window.selected_serial_port(), QString());
        window.serial_ports = {"ttyUSB0"};
        window.serial_port_list->addItem("ttyUSB0");
        QCOMPARE(window.selected_serial_port(), QString("ttyUSB0"));
    }

    void dtcWindowWithoutAPortWarnsInsteadOfCrashing()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        window.serial_ports.clear();
        window.serial_port_list->clear();
        EXPECT_CALL(*services.fake, set_serial_port_list(::testing::_)).Times(0);
        ModalDriver driver{QString()};
        driver.start();
        for (const char *command : {"dtc_window", "biu_communication", "terminal"})
        {
            QVERIFY(QMetaObject::invokeMethod(&window, "menu_action_triggered", Qt::DirectConnection,
                                              Q_ARG(QString, QString::fromLatin1(command))));
        }
        driver.stop();
        QVERIFY(!driver.timedOut());
    }

    void biuWindowRemembersTheOpenedPort()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "K-Line");
        ON_CALL(*services.fake, get_openedSerialPort()).WillByDefault(::testing::Return(QString("ttyUSB0")));
        window.previous_serial_port.clear();
        window.configSession->settings().serial_port = "none";
        window.save_settings();
        const QString config_file =
            config_root_.path() + "/" + QString::fromStdString(kTestApplication.version) + "/config/fastecu.cfg";

        ModalDriver driver{QString()};
        driver.start();
        QVERIFY(triggerMenu(window, "biu_communication"));
        driver.stop();

        // As open_serial_port did for the legacy BIU path: the chosen port is
        // remembered for the next launch and as the previously opened port.
        QCOMPARE(window.previous_serial_port, QString("ttyUSB0"));
        QCOMPARE(window.configSession->settings().serial_port, std::string("ttyUSB0"));
        QFile saved{config_file};
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(saved.readAll().contains(R"(data="ttyUSB0")"));
    }

    void disconnectReturnsTheAdapterToIdle()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        {
            ::testing::InSequence order;
            EXPECT_CALL(*services.fake, reset_connection());
            EXPECT_CALL(*services.fake, set_serial_port_baudrate(QString("4800")));
            EXPECT_CALL(*services.fake, set_serial_port_parity(0));
        }
        EXPECT_CALL(*services.fake, set_is_can_connection(::testing::_)).Times(0);
        QVERIFY(QMetaObject::invokeMethod(&window, "menu_action_triggered", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("disconnect_from_ecu"))));
        // Check now, so facade teardown cannot over-saturate the expectations.
        QVERIFY(::testing::Mock::VerifyAndClearExpectations(services.fake));
        QVERIFY(window.serial_port_list->isEnabled());
    }

    void connectOnAnotherMakeDisconnectsWithoutIdentifying()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Mitsubishi", "K-Line");
        EXPECT_CALL(*services.fake, write_serial_data_echo_check(::testing::_)).Times(0);
        EXPECT_CALL(*services.fake, set_serial_port_parity(0)).Times(::testing::AtLeast(1));

        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(triggerMenu(window, "connect_to_ecu"));
        QVERIFY(elapsed.elapsed() < 1000); // the legacy loop waited 2.5 s here
        QVERIFY(window.identify_worker_ == nullptr);
        QVERIFY(!window.ecu_init_complete);
        QVERIFY(window.serial_port_list->isEnabled());
    }

    void subaruKlineConnectIdentifiesOffTheUiThread()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "K-Line");
        std::atomic<bool> read_off_ui_thread = false;
        EXPECT_CALL(*services.fake, read_serial_data(::testing::_))
            .WillOnce(::testing::Invoke(
                [&window, &read_off_ui_thread](std::uint16_t)
                {
                    read_off_ui_thread.store(QThread::currentThread() != window.thread());
                    return kEcuInit;
                }))
            .WillRepeatedly(::testing::Return(QByteArray{}));

        QVERIFY(triggerMenu(window, "connect_to_ecu"));
        QVERIFY(window.identify_worker_ != nullptr);
        QVERIFY(!window.log_transport_list->isEnabled());
        QVERIFY(!window.serial_port_list->isEnabled());

        constructor_driver.start();
        QTRY_VERIFY_WITH_TIMEOUT(window.identify_worker_ == nullptr, 5000);
        constructor_driver.stop();
        QVERIFY(window.ecu_init_complete);
        QCOMPARE(window.ecuid, QString("3152584006"));
        QVERIFY(read_off_ui_thread.load());
        QVERIFY(window.identify_worker_ == nullptr);
        QVERIFY(window.log_transport_list->isEnabled());
        QVERIFY(!window.serial_port_list->isEnabled()); // stays locked while connected, as before
    }

    void subaruConnectThatNeverAnswersDisconnectsAndRestoresControls()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "K-Line");

        QVERIFY(triggerMenu(window, "connect_to_ecu"));
        QTRY_VERIFY_WITH_TIMEOUT(window.identify_worker_ == nullptr, 15000);
        QVERIFY(!window.ecu_init_complete);
        QVERIFY(window.log_transport_list->isEnabled());
        QVERIFY(window.serial_port_list->isEnabled());
        QVERIFY(window.ecu_radio_button->isEnabled());
        QVERIFY(window.tcu_radio_button->isEnabled());
    }

    void disconnectDuringIdentificationCancelsAndDropsTheResult()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "K-Line");

        EXPECT_CALL(*services.fake, read_serial_data(::testing::_)).WillOnce(::testing::Return(kEcuInit));
        QVERIFY(triggerMenu(window, "connect_to_ecu"));
        QVERIFY(window.identify_worker_ != nullptr);
        // Leave a successful completion queued on the UI thread before cancelling.
        QVERIFY(window.identify_worker_->wait(5000));
        QVERIFY(triggerMenu(window, "disconnect_from_ecu"));
        QVERIFY(window.identify_worker_ == nullptr);
        QVERIFY(window.log_transport_list->isEnabled());
        QVERIFY(window.serial_port_list->isEnabled());
        QVERIFY(window.ecu_radio_button->isEnabled());
        QVERIFY(window.tcu_radio_button->isEnabled());
        QTest::qWait(200); // any completion already queued must be dropped
        QVERIFY(!window.ecu_init_complete);
    }

    void loggingStartWaitsForIdentification_data()
    {
        QTest::addColumn<bool>("target_is_ecu");
        QTest::newRow("ECU") << true;
        QTest::newRow("TCU") << false;
    }

    void loggingStartWaitsForIdentification()
    {
        QFETCH(bool, target_is_ecu);
        QSemaphore response_gate;
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "iso15765");
        window.protocol = "SSM";
        (target_is_ecu ? window.ecu_radio_button : window.tcu_radio_button)->setChecked(true);
        EXPECT_CALL(*services.fake, write_serial_data_echo_check(
                                        frame({0x00, 0x00, 0x07, target_is_ecu ? 0xE0 : 0xE1, 0x22, 0xF1, 0x82})));
        EXPECT_CALL(*services.fake, read_serial_data(::testing::_))
            .WillOnce(::testing::Invoke(
                [&response_gate](std::uint16_t)
                {
                    // Bound the wait so an assertion failure can still join the worker.
                    response_gate.tryAcquire(1, 5000);
                    return frame({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82, 0x12, 0x34, 0x56, 0x78, 0x9A});
                }))
            .WillRepeatedly(::testing::Return(QByteArray{}));
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
        bool target_frozen_in_continuation = false;
        services.logging_engine.registerProtocol(
            "SSM",
            [&window, &target_frozen_in_continuation](const fastecu::desktop::logging::DesktopLoggingSnapshot&)
            {
                target_frozen_in_continuation =
                    !window.ecu_radio_button->isEnabled() && !window.tcu_radio_button->isEnabled();
                auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                protocol->blockPollUntilCancelled();
                return protocol;
            });

        action->setChecked(true);
        QVERIFY(triggerMenu(window, "toggle_realtime"));
        QVERIFY(!window.activeLoggingSnapshot.has_value()); // still identifying
        (target_is_ecu ? window.tcu_radio_button : window.ecu_radio_button)->click();
        response_gate.release();
        QTRY_VERIFY_WITH_TIMEOUT(window.activeLoggingSnapshot.has_value(), 5000);
        QCOMPARE(window.ecuid, QString("123456789A"));
        QCOMPARE(window.activeLoggingSnapshot->target_is_ecu, target_is_ecu);
        QVERIFY(target_frozen_in_continuation);
        QVERIFY(window.ecu_radio_button->isEnabled());
        QVERIFY(window.tcu_radio_button->isEnabled());
        services.logging_engine.stop();
    }

    void batterySamplingDoesNotUseTheFacadeDuringIdentification()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "K-Line");
        QVERIFY(triggerMenu(window, "connect_to_ecu"));
        EXPECT_CALL(*services.fake, get_use_openport2_adapter()).Times(0);
        window.update_vbatt();
        QVERIFY(window.identify_worker_ != nullptr);
        QVERIFY(::testing::Mock::VerifyAndClearExpectations(services.fake));
    }

    void windowDestructionJoinsIdentificationWithoutContinuingLogging()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        auto window = std::make_unique<MainWindow>(services.services());
        constructor_driver.stop();
        prepareConnect(*window, *services.fake, "Subaru", "K-Line");
        bool continued = false;
        window->connect_to_ecu([&continued](bool) { continued = true; });
        QVERIFY(window->identify_worker_ != nullptr);
        window.reset();
        QTest::qWait(200);
        QVERIFY(!continued);
    }

    void connectStopsAnActiveLoggingWorkerBeforeIdentification()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "K-Line");
        services.logging_engine.registerProtocol("SSM",
                                                 [](const fastecu::desktop::logging::DesktopLoggingSnapshot&)
                                                 {
                                                     auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                     protocol->blockPollUntilCancelled();
                                                     return protocol;
                                                 });
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
        QVERIFY(session.has_value());
        fastecu::desktop::logging::DesktopLoggingSnapshot snapshot{.session = std::move(*session)};
        QVERIFY(services.logging_engine.start({"SSM"}, std::move(snapshot)).has_value());
        QTRY_VERIFY(services.logging_engine.isRunning());
        window.logging_state = true;
        QVERIFY(triggerMenu(window, "connect_to_ecu"));
        QVERIFY(window.identify_worker_ != nullptr);
        QVERIFY(!services.logging_engine.isRunning());
        QVERIFY(!window.logging_state);
    }

    void connectionEntryPointsStopIdentification_data()
    {
        QTest::addColumn<QString>("entry_point");
        for (const char *name : {"log_transport_changed", "check_serial_ports", "open_serial_port", "show_dtc_window",
                                 "show_subaru_biu_window", "show_terminal_window"})
        {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void connectionEntryPointsStopIdentification()
    {
        QFETCH(QString, entry_point);
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "K-Line");
        bool cancelled = false;
        window.connect_to_ecu([&cancelled](bool connected) { cancelled = !connected; });
        QVERIFY(window.identify_worker_ != nullptr);
        QVERIFY(!window.serial_port_list->isEnabled());
        QVERIFY(!window.refresh_serial_port_list->isEnabled());
        QTimer close_dialog;
        close_dialog.setInterval(5);
        QObject::connect(&close_dialog, &QTimer::timeout,
                         []
                         {
                             for (QWidget *widget : QApplication::topLevelWidgets())
                             {
                                 if (auto *dialog = qobject_cast<QDialog *>(widget); dialog && dialog->isVisible())
                                 {
                                     dialog->reject();
                                 }
                             }
                         });
        close_dialog.start();
        if (entry_point == "show_dtc_window")
        {
            window.show_dtc_window();
        }
        else if (entry_point == "show_subaru_biu_window")
        {
            window.show_subaru_biu_window();
        }
        else if (entry_point == "show_terminal_window")
        {
            window.show_terminal_window();
        }
        else
        {
            QVERIFY(QMetaObject::invokeMethod(&window, entry_point.toLatin1().constData(), Qt::DirectConnection));
        }
        QVERIFY(window.identify_worker_ == nullptr);
        QVERIFY(cancelled);
        // A cancelled identification leaves no ECU connected, so the port
        // selector unlocks as it does after Disconnect.
        QVERIFY(window.serial_port_list->isEnabled());
        QVERIFY(window.refresh_serial_port_list->isEnabled());
        QTest::qWait(200);
        QVERIFY(!window.ecu_init_complete);
    }

    void nestedConnectDuringCapabilityNoticeKeepsEachContinuation()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "K-Line");
        EXPECT_CALL(*services.fake, read_serial_data(::testing::_))
            .WillOnce(::testing::Return(kEcuInit))
            .WillRepeatedly(::testing::Return(QByteArray{}));
        std::optional<bool> first_result;
        std::optional<bool> second_result;
        window.connect_to_ecu([&first_result](bool connected) { first_result = connected; });
        QVERIFY(window.identify_worker_->wait(5000));
        bool restarted = false;
        bool target_frozen_in_notice = false;
        QTimer notice_driver;
        notice_driver.setInterval(5);
        QObject::connect(&notice_driver, &QTimer::timeout,
                         [&]
                         {
                             for (QWidget *widget : QApplication::topLevelWidgets())
                             {
                                 if (auto *notice = qobject_cast<QMessageBox *>(widget); notice && notice->isVisible())
                                 {
                                     if (!restarted)
                                     {
                                         target_frozen_in_notice = !window.ecu_radio_button->isEnabled() &&
                                                                   !window.tcu_radio_button->isEnabled();
                                         restarted = true;
                                         window.connect_to_ecu([&second_result](bool connected)
                                                               { second_result = connected; });
                                     }
                                     notice->accept();
                                 }
                             }
                         });
        notice_driver.start();
        QCoreApplication::processEvents();
        notice_driver.stop();
        QVERIFY(restarted);
        QVERIFY(target_frozen_in_notice);
        QVERIFY(!window.ecu_radio_button->isEnabled());
        QVERIFY(!window.tcu_radio_button->isEnabled());
        QVERIFY(window.identify_worker_ != nullptr);
        QVERIFY(first_result.has_value());
        QVERIFY(!*first_result);
        QVERIFY(!second_result.has_value());
        window.stop_identification();
        QVERIFY(second_result.has_value());
        QVERIFY(!*second_result);
        QVERIFY(window.ecu_radio_button->isEnabled());
        QVERIFY(window.tcu_radio_button->isEnabled());
    }

  private:
    // The logging setup loggingCapturesTargetForEachRun and
    // loggingUsesTheSessionLogProtocol share: an identified ECU, a
    // "Logging" menu action, and one enabled `log_protocol` value.
    static QAction *prepareLogging(MainWindow& window, const QString& log_protocol)
    {
        window.vbatt_timer->stop();
        window.ecu_init_complete = true;
        window.protocol = log_protocol;
        auto *menu = window.ui->menubar->addMenu("Test logging");
        auto *action = menu->addAction("Logging");
        action->setCheckable(true);
        auto& values = *window.logValues;
        values = FileActions::LogValuesStructure{};
        values.log_value_id = {"rpm"};
        values.log_value_protocol = {log_protocol};
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
        return action;
    }

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
