#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QScopeGuard>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMessageBox>
#include <QRadioButton>
#include <QPushButton>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>
#include <QSemaphore>
#include <QTimer>
#include <QTreeWidget>

#include <gmock/gmock.h>
#include "src/backend/logging/testing/scripted_logging_protocol.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <memory>
#include <initializer_list>
#include <utility>

#include "src/ui/desktop/mainwindow.h"
#include "src/platform/desktop/common/definition/definition_catalog_session.h"
#include "src/backend/logging/logger_definition_service.h"
#include "ui_mainwindow.h"

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
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/rom_open.h"
#include "src/backend/definition/definition_service.h"
#include "src/ui/desktop/calibration/session_key.h"
#include "src/backend/flash/flash_operation_request.h"
#include "src/ui/desktop/calibration/rom_info.h"
#include "src/ui/desktop/calibration_maps.h"
#include "src/backend/calibration/session/rom_save.h"
#include "src/ui/desktop/hexedit/hexedit.h"

namespace
{

const ApplicationIdentity kTestApplication{.name = "FastECU", .title = "FastECU", .version = "0.1.0-beta.5"};

constexpr auto kTcuChooserText = "Choose which option";
constexpr auto kTcuIgnitionText = "Turn ignition ON and press OK to start initializing connection to TCU";
constexpr auto kLegacyEcuIgnitionText = "Turn ignition ON and press OK to start initializing connection to ECU";
constexpr auto kNoChecksumModuleText = "WARNING! There is no checksum module for this ROM!";
constexpr auto kPortableEcuIgnitionText = "Turn ignition ON and press OK to start initializing the ECU connection.";
constexpr auto kContinueWithoutDefinitionText = "Continue without definition file";

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
    int missingDefinitionPromptCount() const
    {
        return missing_definition_prompt_count_;
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

        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (dialog == nullptr || !dialog->isVisible())
            {
                continue;
            }
            for (QRadioButton *button : dialog->findChildren<QRadioButton *>())
            {
                if (button->text() == kContinueWithoutDefinitionText)
                {
                    ++missing_definition_prompt_count_;
                    dialog->reject(); // the continue-without path
                    return;
                }
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
    int missing_definition_prompt_count_ = 0;
    bool timed_out_ = false;
};

// Invoke the slot through Qt, as the menu dispatch does.
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

// A synthetic image on disk for the open-file path.
QString writeRom(const QTemporaryDir& dir, const QString& name, char fill, int size = 64)
{
    const QString path = dir.filePath(name);
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly) || file.write(QByteArray(size, fill)) != size)
    {
        return {};
    }
    return path;
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
          definition_catalogs(definition_service, config, file_system, events)
    {
    }

    MainWindowServices services()
    {
        return {
            .application = kTestApplication,
            .config = config,
            .definition_catalogs = definition_catalogs,
            .calibrations = calibrations,
            .rom_save = rom_save,
            .logger_model = logger_model,
            .logger_definitions = logger_definitions,
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
    fastecu::definition::DefinitionService definition_service{file_system, file_repository, file_writer};
    fastecu::desktop::definition::DefinitionCatalogSession definition_catalogs;
    fastecu::calibration::RomOpenUseCase rom_open{
        definition_catalogs, definition_service, file_repository, file_system, events, config};
    fastecu::calibration::CalibrationWorkspace calibrations{rom_open};
    fastecu::calibration::RomSaveUseCase rom_save{file_repository, events};
    fastecu::logging::LoggerModel logger_model;
    fastecu::logging::LoggerDefinitionService logger_definitions{file_repository, resource_bundle, file_writer};
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
        QVERIFY(window.calibrations_.empty());
        QVERIFY(services.calibrations.ids().empty());
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
    void readOfAnUnsupportedProtocolAddsNoCalibration()
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
        QCOMPARE(window.calibrations_.size(), std::size_t{0});

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "read"), 0);
        operation_driver.stop();

        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(window.calibrations_.size(), std::size_t{0});
        QVERIFY(services.calibrations.ids().empty());
        QCOMPARE(window.ui->calibrationFilesTreeWidget->topLevelItemCount(), 0);
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
        QTemporaryDir roms;
        const QString rom_path = writeRom(roms, "test.bin", '\x5a', 16);
        QVERIFY(!rom_path.isEmpty());
        ModalDriver open_driver{QString()};
        open_driver.start();
        QCOMPARE(window.open_calibration_file(rom_path), 0);
        open_driver.stop();
        QCOMPARE(open_driver.missingDefinitionPromptCount(), 1);
        selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can_checksum_na");

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "write"), 0);
        operation_driver.stop();

        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(operation_driver.checksumWarningCount(), 1);
        QVERIFY(std::ranges::all_of(services.calibrations.find(window.calibrations_.front().id)->rom(),
                                    [](auto byte) { return byte == 0x5a; }));
        QVERIFY(!window.vbatt_timer->isActive());
    }

    void definitionlessOpenPromptsOnceAndAppliesPlaceholders()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QTemporaryDir roms;
        const QString path = writeRom(roms, "a.bin", '\x11');

        ModalDriver driver{QString()};
        driver.start();
        QCOMPARE(window.open_calibration_file(path), 0);
        driver.stop();

        QVERIFY(!driver.timedOut());
        QCOMPARE(driver.missingDefinitionPromptCount(), 1);
        QCOMPARE(window.calibrations_.size(), std::size_t{1});
        QTreeWidgetItem *rom_info = window.ui->calibrationDataTreeWidget->topLevelItem(0);
        QCOMPARE(rom_info->text(0), QString("ROM Info"));
        QCOMPARE(rom_info->child(0)->text(0), QString("XML ID: UnknownID"));
        QCOMPARE(rom_info->child(4)->text(0),
                 "Make: " + QString::fromStdString(services.config.selected_vehicle()->make));
        QCOMPARE(window.calibrations_.front().view.missing_definition_make,
                 std::optional<QString>(QString::fromStdString(services.config.selected_vehicle()->make)));
        QCOMPARE(services.calibrations.find(window.calibrations_.front().id)->source().display_name,
                 std::string{"a.bin"});
        QCOMPARE(services.calibrations.ids().size(), std::size_t{1});
        QCOMPARE(window.ui->calibrationFilesTreeWidget->topLevelItem(0)->text(2),
                 fastecu::ui::session_key_text(window.calibrations_.front().id));
    }

    void closingAMiddleRomKeepsLaterRomsAddressable()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QTemporaryDir roms;
        ModalDriver driver{QString()};
        driver.start();
        QCOMPARE(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
        QCOMPARE(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
        QCOMPARE(window.open_calibration_file(writeRom(roms, "c.bin", '\x0c')), 0);
        driver.stop();
        QCOMPARE(window.calibrations_.size(), std::size_t{3});
        const auto a = window.calibrations_.at(0).id;
        const auto c = window.calibrations_.at(2).id;
        QTreeWidget *files = window.ui->calibrationFilesTreeWidget;
        const QString c_key = files->topLevelItem(2)->text(2);

        for (int i = 0; i < files->topLevelItemCount(); ++i)
        {
            files->topLevelItem(i)->setSelected(i == 1);
        }
        window.close_calibration();

        QCOMPARE(window.calibrations_.size(), std::size_t{2});
        QCOMPARE(services.calibrations.ids(), (std::vector{a, c}));
        QCOMPARE(files->topLevelItemCount(), 2);
        QCOMPARE(files->topLevelItem(1)->text(2), c_key); // not renumbered
        QVERIFY(services.calibrations.find(c) != nullptr);
        QCOMPARE(services.calibrations.find(c)->source().display_name, std::string{"c.bin"});
        QCOMPARE(services.calibrations.find(c)->rom()[0], std::uint8_t{0x0c});
    }

    void windowsOfAClosedRomAreInert()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QTemporaryDir roms;
        ModalDriver driver{QString()};
        driver.start();
        QCOMPARE(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
        QCOMPARE(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
        driver.stop();
        const auto b = window.calibrations_.at(1).id;
        window.close_calibration(); // b is selected after its open
        QVERIFY(services.calibrations.find(b) == nullptr);

        const QString stale = fastecu::ui::session_key_text(b) + ",0,Idle";
        auto *content = new QWidget;
        QMdiSubWindow *sub = window.ui->mdiArea->addSubWindow(content);
        sub->setObjectName(stale);
        content->setObjectName(stale);
        window.ui->mdiArea->setActiveSubWindow(sub);
        QObject destroyed_window;
        destroyed_window.setObjectName(stale);

        window.set_maptablewidget_items();
        window.selectable_combobox_item_changed("anything");
        window.checkbox_state_changed(2);
        window.close_calibration_map(&destroyed_window);

        QCOMPARE(window.calibrations_.size(), std::size_t{1});
        QCOMPARE(window.ui->calibrationFilesTreeWidget->topLevelItemCount(), 1);
    }

    void hexEditorOutlivesItsRom()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QTemporaryDir roms;
        ModalDriver driver{QString()};
        driver.start();
        QCOMPARE(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
        driver.stop();

        window.show_hex_editor();
        window.close_calibration();

        QCOMPARE(window.findChildren<HexEdit *>().size(), qsizetype{1});
        QVERIFY(window.calibrations_.empty());
    }

    void closingARomClosesAllOfItsWindows()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QTemporaryDir roms;
        ModalDriver driver{QString()};
        driver.start();
        QCOMPARE(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
        QCOMPARE(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
        driver.stop();
        const QString a_key = fastecu::ui::session_key_text(window.calibrations_.at(0).id);
        const QString b_key = fastecu::ui::session_key_text(window.calibrations_.at(1).id);
        for (const QString& name : {a_key + ",0,X", a_key + ",1,Y", b_key + ",0,Z"})
        {
            auto *content = new QWidget;
            QMdiSubWindow *sub = window.ui->mdiArea->addSubWindow(content);
            sub->setObjectName(name);
        }
        // The data tree still shows b; select a's row directly, as keyboard
        // navigation would, without rebuilding the data tree.
        QTreeWidget *files = window.ui->calibrationFilesTreeWidget;
        files->topLevelItem(0)->setSelected(true);
        files->topLevelItem(1)->setSelected(false);

        window.close_calibration();

        QStringList remaining;
        for (QMdiSubWindow *sub : window.ui->mdiArea->subWindowList())
        {
            remaining << sub->objectName();
        }
        QCOMPARE(remaining, QStringList{b_key + ",0,Z"});
    }

    void viewStateIsKeptPerRom()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QTemporaryDir roms;
        ModalDriver driver{QString()};
        driver.start();
        QCOMPARE(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
        QCOMPARE(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
        driver.stop();
        QTreeWidget *files = window.ui->calibrationFilesTreeWidget;
        QTreeWidget *data = window.ui->calibrationDataTreeWidget;
        const auto select_rom = [&](int row)
        {
            for (int i = 0; i < files->topLevelItemCount(); ++i)
            {
                files->topLevelItem(i)->setSelected(i == row);
            }
            window.calibration_files_treewidget_item_selected(files->topLevelItem(row));
        };

        select_rom(0);
        window.calibration_data_treewidget_item_expanded(data->topLevelItem(0)); // ROM Info
        select_rom(1);
        QVERIFY(!data->topLevelItem(0)->isExpanded());
        select_rom(0);
        QVERIFY(data->topLevelItem(0)->isExpanded());
        QVERIFY(window.calibrations_.at(0).view.rom_info_expanded);
        QVERIFY(!window.calibrations_.at(1).view.rom_info_expanded);
    }

    // The write path's metadata refresh decides the kernel and MCU handed to
    // a real ECU, so it is tested directly: an empty definition flash method
    // is filled from the selected vehicle (and the vehicle re-selected by
    // it); a definition-less ROM (" ") is not; kernel and MCU always come
    // from the vehicle selected afterwards.
    void writeMetadataFillsAnEmptyDefinitionFlashMethod()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can_checksum_na");
        const std::string selected = services.config.selected_vehicle()->protocol_name;
        fastecu::calibration::CalibrationSession session(
            fastecu::calibration::SessionId{41},
            fastecu::calibration::SessionContents{
                .source = {.display_name = "d.bin", .path = "/d.bin"},
                .rom = std::vector<std::uint8_t>(16, 0),
                .definition =
                    fastecu::calibration::ResolvedDefinition{
                        .id = "D", .definition = {.format = fastecu::definition::DefinitionFormat::EcuFlash}},
            });
        QCOMPARE(
            fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(session), fastecu::ui::RomInfoRow::FlashMethod),
            QString(""));

        window.refresh_write_metadata(session, "/kernels/");

        const auto& vehicle = *services.config.selected_vehicle();
        QCOMPARE(session.protocol().flash_method, selected);
        QCOMPARE(vehicle.protocol_name, selected);
        QCOMPARE(session.protocol().mcu_type,
                 fastecu::config::protocol_field_or_placeholder(vehicle, &fastecu::config::ProtocolEntry::mcu));
        QCOMPARE(session.protocol().kernel_path,
                 fastecu::flash::kernel_path("/kernels/", fastecu::config::protocol_field_or_placeholder(
                                                              vehicle, &fastecu::config::ProtocolEntry::kernel)));
        QCOMPARE(
            fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(session), fastecu::ui::RomInfoRow::FlashMethod),
            QString::fromStdString(selected));
    }

    void writeMetadataLeavesADefinitionlessFlashMethodAlone()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can_checksum_na");
        fastecu::calibration::CalibrationSession session(
            fastecu::calibration::SessionId{42},
            fastecu::calibration::SessionContents{.source = {.display_name = "n.bin", .path = "/n.bin"},
                                                  .rom = std::vector<std::uint8_t>(16, 0)});

        window.refresh_write_metadata(session, "/kernels/");

        const auto& vehicle = *services.config.selected_vehicle();
        QCOMPARE(session.protocol().flash_method, std::string{});
        QCOMPARE(
            fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(session), fastecu::ui::RomInfoRow::FlashMethod),
            QString(" "));
        QCOMPARE(session.protocol().mcu_type,
                 fastecu::config::protocol_field_or_placeholder(vehicle, &fastecu::config::ProtocolEntry::mcu));
    }

    void checksumAndSaveUseATemporaryImage()
    {
        ModalDriver driver{QString()};
        driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        selectSubaruProtocol(window, "sub_ecu_denso_sh7058");
        QTemporaryDir files;
        QVERIFY(writeTextFile(
            files.path() + "/definition.xml",
            R"(<rom><romid><xmlid>SAVE</xmlid><flashmethod>sub_ecu_denso_sh7058</flashmethod></romid></rom>)"));
        services.config.settings().primary_definition_base = "ecuflash";
        services.config.settings().use_ecuflash_definitions = "enabled";
        services.config.settings().ecuflash_definition_files_directory = files.path().toStdString();
        QVERIFY(
            services.definition_catalogs.refresh_index(fastecu::definition::DefinitionFormat::EcuFlash).has_value());
        const QString path = files.path() + "/save.bin";
        const auto opened = services.calibrations.adopt_read_image({
            .rom = bytes::Bytes(1024UZ * 1024, 0),
            .filename = path.toStdString(),
            .rom_id = "SAVE",
            .protocol_name = "sub_ecu_denso_sh7058",
        });
        QVERIFY(opened.has_value());
        QVERIFY(window.add_calibration(opened->id));
        selectSubaruProtocol(window, "sub_ecu_denso_sh7058");
        auto *session = services.calibrations.find(opened->id);
        QVERIFY(session != nullptr);
        QVERIFY(session->definition() != nullptr);
        QVERIFY(session->write_bytes(0, bytes::Bytes{1}).has_value());
        const bytes::Bytes original(session->rom().begin(), session->rom().end());
        bytes::Bytes operation_image = original;

        window.runChecksumCorrection(*session, operation_image);

        QVERIFY(operation_image != original);
        QVERIFY(std::ranges::equal(session->rom(), original));
        QVERIFY(session->dirty());
        window.save_calibration_file();
        const auto saved = services.file_repository.read(path.toStdString());
        QVERIFY(saved.has_value());
        QVERIFY(*saved == operation_image);
        QVERIFY(std::ranges::equal(session->rom(), original));
        QVERIFY(!session->dirty());
        QVERIFY(session->write_bytes(0, bytes::Bytes{2}).has_value());
        const bytes::Bytes edited(session->rom().begin(), session->rom().end());
        // A directory is a deterministic failed file write on every platform.
        session->mark_saved(files.path().toStdString());
        QVERIFY(session->write_bytes(0, bytes::Bytes{2}).has_value());
        const auto source = session->source();
        window.save_calibration_file();
        QVERIFY(session->source() == source);
        QVERIFY(session->dirty());
        QVERIFY(std::ranges::equal(session->rom(), edited));
        driver.stop();
        QVERIFY(!driver.timedOut());
    }

    void saveAsChangesSourceAndTreeOnlyAfterSuccess()
    {
        const bool native_disabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto restore_dialogs =
            qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native_disabled); });
        ModalDriver driver{QString()};
        driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        QTemporaryDir files;
        const QString original_path = writeRom(files, "original.bin", '\x11');
        QCOMPARE(window.open_calibration_file(original_path), 0);
        auto *session = services.calibrations.find(window.calibrations_.front().id);
        QVERIFY(session != nullptr);
        auto protocol = session->protocol();
        protocol.mcu_type.clear(); // Unknown MCU preserves bytes without a checksum dialog.
        session->set_protocol(protocol);
        QVERIFY(session->write_bytes(0, bytes::Bytes{9}).has_value());
        const auto original_source = session->source();
        QTreeWidgetItem *row = window.ui->calibrationFilesTreeWidget->topLevelItem(0);
        const QString original_label = row->text(0);
        driver.stop();

        const auto save_as = [&](const QString& target, bool cancel, bool fail)
        {
            QTimer timer;
            timer.setInterval(5);
            bool handled = false;
            bool timed_out = false;
            QElapsedTimer deadline;
            deadline.start();
            QObject::connect(
                &timer, &QTimer::timeout, &window,
                [&]
                {
                    for (auto *widget : QApplication::topLevelWidgets())
                    {
                        auto *dialog = qobject_cast<QFileDialog *>(widget);
                        if (dialog == nullptr || !dialog->isVisible())
                        {
                            continue;
                        }
                        if (deadline.elapsed() > 3000)
                        {
                            timed_out = true;
                            dialog->reject();
                            return;
                        }
                        if (handled)
                        {
                            continue;
                        }
                        handled = true;
                        if (cancel)
                        {
                            dialog->reject();
                            return;
                        }
                        dialog->setDirectory(QFileInfo(target).absolutePath());
                        dialog->selectFile(QFileInfo(target).fileName());
                        if (auto *edit = dialog->findChild<QLineEdit *>("fileNameEdit"); edit != nullptr)
                        {
                            edit->setText(QFileInfo(target).fileName());
                        }
                        if (fail)
                        {
                            QObject::connect(dialog, &QDialog::accepted, &window, [target] { QDir().mkdir(target); });
                        }
                        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                    }
                    // Close the existing cancellation/failure informational notice.
                    for (auto *widget : QApplication::topLevelWidgets())
                    {
                        if (auto *box = qobject_cast<QMessageBox *>(widget); box != nullptr && box->isVisible())
                        {
                            box->accept();
                        }
                    }
                });
            timer.start();
            window.save_calibration_file_as();
            timer.stop();
            if (timed_out)
            {
                qWarning() << "Save As dialog timed out for" << target;
            }
            return handled && !timed_out;
        };

        QVERIFY(save_as({}, true, false));
        QVERIFY(session->source() == original_source);
        QVERIFY(session->dirty());
        QCOMPARE(row->text(0), original_label);
        const QString blocked = files.path() + "/blocked.bin";
        QVERIFY(save_as(blocked, false, true));
        QVERIFY(session->source() == original_source);
        QVERIFY(session->dirty());
        QCOMPARE(row->text(0), original_label);
        QVERIFY(save_as(files.path() + "/renamed.", false, false));
        QCOMPARE(session->source().path, (files.path() + "/renamed.bin").toStdString());
        QCOMPARE(session->source().display_name, std::string{"renamed.bin"});
        QVERIFY(!session->dirty());
        QVERIFY(row->text(0).contains("renamed.bin"));
        const auto saved = services.file_repository.read(session->source().path);
        QVERIFY(saved.has_value());
        QCOMPARE(saved->at(0), std::uint8_t{9});
        QCOMPARE(session->rom()[0], std::uint8_t{9});
    }

    void selectableSignalEditsItsEmittingSession()
    {
        ModalDriver driver{QString()};
        driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        window.show();
        QApplication::processEvents();
        QTemporaryDir files;
        QVERIFY(writeTextFile(files.path() + "/selector.xml", R"(
<rom><romid><xmlid>SELECT</xmlid></romid>
<table name="Mode" category="Controls" address="0" type="1D" sizex="1" sizey="1">
<scaling storagetype="bloblist" endian="big"><data name="off" value="00"/><data name="on" value="01"/></scaling>
</table></rom>)"));
        services.config.settings().primary_definition_base = "ecuflash";
        services.config.settings().use_ecuflash_definitions = "enabled";
        services.config.settings().ecuflash_definition_files_directory = files.path().toStdString();
        QVERIFY(
            services.definition_catalogs.refresh_index(fastecu::definition::DefinitionFormat::EcuFlash).has_value());
        const auto open_map = [&](const QString& name) -> CalibrationMaps *
        {
            const auto opened = services.calibrations.adopt_read_image({
                .rom = bytes::Bytes(16, 0),
                .filename = name.toStdString(),
                .rom_id = "SELECT",
            });
            if (!opened.has_value() || !window.add_calibration(opened->id))
            {
                return nullptr;
            }
            auto *file_tree = window.ui->calibrationFilesTreeWidget;
            for (int row = 0; row < file_tree->topLevelItemCount(); ++row)
            {
                file_tree->topLevelItem(row)->setSelected(row == file_tree->topLevelItemCount() - 1);
            }
            window.calibration_files_treewidget_item_selected(
                file_tree->topLevelItem(file_tree->topLevelItemCount() - 1));
            QTreeWidget *tree = window.ui->calibrationDataTreeWidget;
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
            {
                auto *category = tree->topLevelItem(i);
                if (category->text(0) == "Controls" && category->childCount() != 0)
                {
                    tree->setCurrentItem(category->child(0));
                    window.calibration_data_treewidget_item_selected(category->child(0));
                    const auto windows = window.ui->mdiArea->subWindowList();
                    if (windows.isEmpty())
                    {
                        return nullptr;
                    }
                    window.ui->mdiArea->setActiveSubWindow(windows.back());
                    return qobject_cast<CalibrationMaps *>(windows.back()->widget());
                }
            }
            return nullptr;
        };
        CalibrationMaps *first = open_map(files.path() + "/first.bin");
        QVERIFY(first != nullptr);
        const auto first_id = services.calibrations.ids().front();
        CalibrationMaps *second = open_map(files.path() + "/second.bin");
        QVERIFY(second != nullptr);
        QVERIFY(first != second);
        const auto second_id = services.calibrations.ids().back();
        QVERIFY(window.ui->mdiArea->activeSubWindow()->widget() == second);

        // Emit from the inactive first map while the second ROM/window is selected.
        first->selectable_combobox_item_changed("enabled");

        QCOMPARE(services.calibrations.find(first_id)->rom()[0], std::uint8_t{1});
        QVERIFY(services.calibrations.find(first_id)->dirty());
        QCOMPARE(services.calibrations.find(second_id)->rom()[0], std::uint8_t{0});
        QVERIFY(!services.calibrations.find(second_id)->dirty());
        driver.stop();
        QVERIFY(!driver.timedOut());
    }

    void failedMapDecodeDoesNotOccupyAView()
    {
        ModalDriver driver{QString()};
        driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        QTemporaryDir files;
        QCOMPARE(window.open_calibration_file(writeRom(files, "bad.bin", '\x11')), 0);
        driver.stop();
        const auto id = window.calibrations_.front().id;
        auto *session = services.calibrations.find(id);
        QVERIFY(session != nullptr);
        fastecu::definition::RomDefinition definition{.format = fastecu::definition::DefinitionFormat::EcuFlash};
        definition.scalings.push_back({.name = "Raw"});
        fastecu::definition::CalibrationMap map;
        map.name = "Broken";
        map.category = "Controls";
        map.type = "1D";
        map.address = 1000; // Beyond the opened 16-byte image.
        map.x_size = 1;
        map.y_size = 1;
        map.storage_type = fastecu::definition::StorageType::Uint8;
        map.scaling_name = "Raw";
        definition.maps.push_back(map);
        *session = fastecu::calibration::CalibrationSession(
            id, {
                    .source = session->source(),
                    .rom = bytes::Bytes(16, 0),
                    .definition =
                        fastecu::calibration::ResolvedDefinition{.id = "BAD", .definition = std::move(definition)},
                });
        window.calibration_files_treewidget_item_selected(window.ui->calibrationFilesTreeWidget->topLevelItem(0));
        QTreeWidget *tree = window.ui->calibrationDataTreeWidget;
        QTreeWidgetItem *item = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
        {
            auto *category = tree->topLevelItem(i);
            if (category->text(0) == "Controls")
            {
                item = category->child(0);
            }
        }
        QVERIFY(item != nullptr);
        QSignalSpy errors(&window, &MainWindow::LOG_E);
        tree->setCurrentItem(item);
        window.calibration_data_treewidget_item_selected(item);
        QVERIFY(window.ui->mdiArea->subWindowList().isEmpty());
        QVERIFY(window.calibrations_.front().view.open_maps.empty());
        QCOMPARE(item->checkState(0), Qt::Unchecked);
        QVERIFY(!errors.isEmpty());
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

    void chooserDialogsApplyAcceptedChoicesAndIgnoreCancellation_data()
    {
        QTest::addColumn<bool>("protocol");
        QTest::addColumn<bool>("accept");
        QTest::newRow("vehicle-accept") << false << true;
        QTest::newRow("vehicle-cancel") << false << false;
        QTest::newRow("protocol-accept") << true << true;
        QTest::newRow("protocol-cancel") << true << false;
    }

    void chooserDialogsApplyAcceptedChoicesAndIgnoreCancellation()
    {
        QFETCH(bool, protocol);
        QFETCH(bool, accept);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        copyFixtureConfig(root.path());
        TestServices services{root.path()};
        QVERIFY(services.config_status.has_value());
        QVERIFY(services.config.select_row(0).has_value());
        QVERIFY(services.config.save().has_value());
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        MainWindow window{services.services()};
        constructor_driver.stop();
        const auto target = services.config.vehicles()[1];
        std::size_t expected = 1;
        if (protocol)
        {
            for (std::size_t row = 0; row < services.config.vehicles().size(); ++row)
            {
                if (services.config.vehicles()[row].protocol_name == target.protocol_name)
                {
                    expected = row;
                }
            }
        }
        QTimer driver;
        QElapsedTimer deadline;
        bool driven = false;
        bool unexpected = false;
        bool timed_out = false;
        QObject::connect(&driver, &QTimer::timeout,
                         [&]
                         {
                             auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                             if (deadline.elapsed() > 3000)
                             {
                                 timed_out = true;
                                 if (dialog)
                                 {
                                     dialog->reject();
                                 }
                                 return;
                             }
                             if (!dialog)
                             {
                                 return;
                             }
                             const char *expected_class = protocol ? "ProtocolSelect" : "VehicleSelect";
                             if (!dialog->inherits(expected_class))
                             {
                                 unexpected = true;
                                 dialog->reject();
                                 return;
                             }
                             auto selectText = [](QTreeWidget *tree, const QString& text)
                             {
                                 if (!tree)
                                 {
                                     return false;
                                 }
                                 for (int i = 0; i < tree->topLevelItemCount(); ++i)
                                 {
                                     if (tree->topLevelItem(i)->text(0) == text)
                                     {
                                         tree->setCurrentItem(tree->topLevelItem(i));
                                         return true;
                                     }
                                 }
                                 return false;
                             };
                             bool selected = false;
                             if (protocol)
                             {
                                 selected = selectText(dialog->findChild<QTreeWidget *>("treeWidget"),
                                                       QString::fromStdString(target.protocol_name));
                             }
                             else
                             {
                                 selected = selectText(dialog->findChild<QTreeWidget *>("car_make_tree_widget"),
                                                       QString::fromStdString(target.make)) &&
                                            selectText(dialog->findChild<QTreeWidget *>("car_model_tree_widget"),
                                                       QString::fromStdString(target.model));
                                 auto *versions = dialog->findChild<QTreeWidget *>("car_version_tree_widget");
                                 selected = selected && versions != nullptr;
                                 bool row_found = false;
                                 if (versions)
                                 {
                                     for (int i = 0; i < versions->topLevelItemCount(); ++i)
                                     {
                                         auto *item = versions->topLevelItem(i);
                                         if (item->text(12) == "1")
                                         {
                                             versions->setCurrentItem(item);
                                             row_found = true;
                                             break;
                                         }
                                     }
                                 }
                                 selected = selected && row_found;
                             }
                             auto *button =
                                 dialog->findChild<QPushButton *>(accept ? "select_button" : "cancel_button");
                             driven = selected && button && button->isEnabled();
                             if (driven)
                             {
                                 button->click();
                             }
                             else
                             {
                                 unexpected = true;
                                 dialog->reject();
                             }
                         });
        deadline.start();
        driver.start(5);
        if (protocol)
        {
            window.select_protocol();
        }
        else
        {
            window.select_vehicle();
        }
        driver.stop();
        QVERIFY(driven);
        QVERIFY(!unexpected);
        QVERIFY(!timed_out);
        QCOMPARE(*services.config.selected_row(), accept ? expected : std::size_t{0});
        TestServices reread{root.path()};
        QVERIFY(reread.config_status.has_value());
        QCOMPARE(*reread.config.selected_row(), accept ? expected : std::size_t{0});
    }

    void definitionManagerRemovesSelectedRowsAndSavesSurvivingOrder()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        copyFixtureConfig(root.path());
        TestServices services{root.path()};
        QVERIFY(services.config_status.has_value());
        services.config.settings().romraider_definition_files = {"/first.xml", "/middle.xml", "/last.xml"};
        QVERIFY(services.config.save().has_value());
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        MainWindow window{services.services()};
        constructor_driver.stop();
        QTimer driver;
        QElapsedTimer deadline;
        bool driven = false;
        bool unexpected = false;
        bool unchanged_without_selection = false;
        QStringList displayed;
        QObject::connect(&driver, &QTimer::timeout,
                         [&]
                         {
                             auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                             if (!dialog)
                             {
                                 return;
                             }
                             if (deadline.elapsed() > 3000 || dialog->objectName() != "ecu_definition_manager_dialog")
                             {
                                 unexpected = true;
                                 dialog->reject();
                                 return;
                             }
                             auto *list = dialog->findChild<QListWidget *>("ecu_definition_files_list");
                             QPushButton *remove = nullptr;
                             for (auto *button : dialog->findChildren<QPushButton *>())
                             {
                                 if (button->text() == "Remove file")
                                 {
                                     remove = button;
                                 }
                             }
                             if (!list || !remove || list->count() != 3)
                             {
                                 unexpected = true;
                                 dialog->reject();
                                 return;
                             }
                             list->clearSelection();
                             remove->click();
                             unchanged_without_selection =
                                 services.config.settings().romraider_definition_files ==
                                 std::vector<std::string>{"/first.xml", "/middle.xml", "/last.xml"};
                             list->item(1)->setSelected(true);
                             remove->click();
                             for (int i = 0; i < list->count(); ++i)
                             {
                                 displayed.append(list->item(i)->text());
                             }
                             driven = true;
                             dialog->reject();
                         });
        deadline.start();
        driver.start(5);
        window.ecu_definition_manager();
        driver.stop();
        QVERIFY(driven);
        QVERIFY(!unexpected);
        QVERIFY(unchanged_without_selection);
        QCOMPARE(displayed, (QStringList{"/first.xml", "/last.xml"}));
        const std::vector<std::string> expected{"/first.xml", "/last.xml"};
        QCOMPARE(services.config.settings().romraider_definition_files, expected);
        TestServices reread{root.path()};
        QVERIFY(reread.config_status.has_value());
        QCOMPARE(reread.config.settings().romraider_definition_files, expected);
    }

    void numericWindowGeometryRestoresAndPersistsAcrossWindowStates()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        copyFixtureConfig(root.path());
        TestServices services{root.path()};
        QVERIFY(services.config_status.has_value());
        services.config.settings().window_width = "900";
        services.config.settings().window_height = "700";
        QVERIFY(services.config.save().has_value());
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        MainWindow window{services.services()};
        constructor_driver.stop();
        QCOMPARE(window.size(), QSize(900, 700));
        window.show();
        window.resize(950, 750);
        QCoreApplication::processEvents();
        QCOMPARE(window.size(), QSize(950, 750));
        TestServices resized{root.path()};
        QVERIFY(resized.config_status.has_value());
        QCOMPARE(resized.config.settings().window_width, std::string("950"));
        QCOMPARE(resized.config.settings().window_height, std::string("750"));
        window.showMaximized();
        QCoreApplication::processEvents();
        QCOMPARE(services.config.settings().window_width, std::string("maximized"));
        QCOMPARE(services.config.settings().window_height, std::string("maximized"));
        TestServices maximized{root.path()};
        QVERIFY(maximized.config_status.has_value());
        QCOMPARE(maximized.config.settings().window_width, std::string("maximized"));
        window.showNormal();
        QCoreApplication::processEvents();
        QCOMPARE(services.config.settings().window_width, std::to_string(window.width()));
        QCOMPARE(services.config.settings().window_height, std::to_string(window.height()));
        TestServices restored{root.path()};
        QVERIFY(restored.config_status.has_value());
        QCOMPARE(restored.config.settings().window_width, services.config.settings().window_width);
        QCOMPARE(restored.config.settings().window_height, services.config.settings().window_height);
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
        window.update_protocol_info("sub_ecu_denso_sh7058");

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
        window.update_protocol_info("no_such_protocol");

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

    void repeatedSaveFailuresLogOnceUntilASuccess()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        QTemporaryDir root;
        QVERIFY(root.isValid());
        TestServices services{root.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QSignalSpy errors{&window, &MainWindow::LOG_E};
        const QString config_file = QString::fromStdString(services.config.provisioned_paths().config_file);
        QVERIFY(QFile::remove(config_file));
        QVERIFY(QDir().mkpath(config_file));

        services.config.settings().toolbar_iconsize = "48";
        window.save_settings();
        window.save_settings();
        window.save_settings();
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.front().front().toString().contains(config_file));
        QCOMPARE(services.config.settings().toolbar_iconsize, std::string("48"));

        QVERIFY(QDir().rmdir(config_file));
        window.save_settings();
        QCOMPARE(errors.size(), 1);
        QFile saved{config_file};
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(saved.readAll().contains(R"(data="48")"));
        saved.close();
        QVERIFY(QFile::remove(config_file));
        QVERIFY(QDir().mkpath(config_file));
        window.save_settings();
        QCOMPARE(errors.size(), 2);
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

    // Synthetic fixtures reproduced legacy label/ID lookups in 6l-1.
    // These assertions pin the corrected identities and empty CSV cells.
    void loggingSelectionFailureSemanticsAndSupportPreservation()
    {
        ModalDriver driver{QString()};
        driver.start();
        TestServices services{config_root_.path()};
        MainWindow window{services.services()};
        window.vbatt_timer->stop();
        const auto cfg = QString::fromStdString(services.config.effective_paths().logger_file);
        window.ecuid = "MODEL_TEST";
        installLoggingFixture(
            window,
            {.parameters = {{.protocol = "SSM", .id = "rpm", .ecu_byte_index = "0", .ecu_bit = "0", .enabled = true}},
             .switches = {{.protocol = "SSM", .id = "flag", .ecu_byte_index = "5", .enabled = true}}},
            {.protocol = "SSM", .gauge_ids = {"old"}, .lower_panel_ids = {"old"}, .switch_ids = {"old"}});
        const auto previous = window.loggerModel->selection();
        QVERIFY(QFile::remove(cfg));
        window.load_logger_selection();
        QVERIFY(window.loggerModel->selection() == previous);
        window.loggerModel->set_selection({.protocol = "SSM", .gauge_ids = {"operator-edit", "unresolved"}});
        const auto edited = window.loggerModel->selection();
        window.save_logger_selection();
        QVERIFY(window.loggerModel->selection() == edited);
        QVERIFY(writeTextFile(cfg, "<config><logger/></config>"));
        window.loggerModel->set_parameter_supported("SSM", "rpm", false);
        window.load_logger_selection();
        QVERIFY(window.loggerModel->selection().gauge_ids.empty());
        QCOMPARE(window.loggerModel->selection().switch_ids, (std::vector<std::string>{"flag"}));
        QVERIFY(!window.loggerModel->parameter_supported("SSM", "rpm"));
        QVERIFY(writeTextFile(
            cfg,
            R"(<config><logger><ecu id="MODEL_TEST"><protocol id="SSM"><parameters><gauges><parameter id="unknown"/></gauges><lower_panel><parameter id="rpm"/></lower_panel></parameters><switches><switch id="flag"/></switches></protocol></ecu></logger></config>)"));
        window.load_logger_selection();
        QCOMPARE(window.loggerModel->selection().gauge_ids, (std::vector<std::string>{"unknown"}));
        QVERIFY(!window.loggerModel->parameter_supported("SSM", "rpm"));
        // A valid capability byte updates parameters; missing switch bytes retain flags.
        window.parse_log_value_list(frame({0, 0, 0, 0, 0, 1}), "SSM");
        QVERIFY(window.loggerModel->parameter_supported("SSM", "rpm"));
        QVERIFY(window.loggerModel->switch_supported("SSM", "flag"));
        QVERIFY(window.loggerModel->definition().parameters.front().enabled);
        window.save_logger_selection();
        const auto stored = services.logger_definitions.load_selection(cfg.toStdString(), "MODEL_TEST");
        QVERIFY(stored.has_value());
        QVERIFY(stored->has_value());
        QVERIFY(**stored == window.loggerModel->selection());
        // Missing definitions clear stale IDs after a successful read and never persist defaults.
        installLoggingFixture(window, {}, {.protocol = "SSM", .lower_panel_ids = {"stale"}});
        QVERIFY(writeTextFile(cfg, "<config><logger/></config>"));
        window.load_logger_selection();
        QVERIFY(window.loggerModel->selection().lower_panel_ids.empty());
        QFile conf{cfg};
        QVERIFY(conf.open(QIODevice::ReadOnly));
        QCOMPARE(conf.readAll(), QByteArray("<config><logger/></config>"));
        driver.stop();
    }

    void loggingDefinitionFailureIsNonfatal()
    {
        ModalDriver driver{QString()};
        driver.start();
        TestServices services{config_root_.path()};
        services.config.settings().romraider_logger_definition_file = "/missing/logger.xml";
        MainWindow window{services.services()};
        QVERIFY(window.loggerModel->definition().parameters.empty());
        QVERIFY(window.loggerModel->selection().lower_panel_ids.empty());
        QVERIFY(window.ui != nullptr);
        driver.stop();
    }

    void unresolvedDisplaySlotsAreSkippedAndUpdateTheirOriginalLabels()
    {
        ModalDriver driver{QString()};
        driver.start();
        TestServices services{config_root_.path()};
        MainWindow window{services.services()};
        driver.stop();
        installLoggingFixture(window,
                              {.parameters = {{.protocol = "SSM",
                                               .id = "rpm",
                                               .name = "Speed",
                                               .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}}}},
                              {.protocol = "SSM", .lower_panel_ids = {"unresolved", "rpm", "another-unresolved"}});
        window.update_logboxes("SSM");
        QCOMPARE(window.ui->logBoxLayout->count(), 1);
        QVERIFY(window.loggerValues.set_parameter_value({"SSM", "rpm"}, "123.00"));
        window.update_logbox_values("SSM");
        const auto *label = window.findChild<QLabel *>("log_label1");
        QVERIFY(label != nullptr);
        QCOMPARE(label->text(), QString("123.00 <font size=1px color=grey>rpm</font>"));
        QCOMPARE(label->alignment(), Qt::Alignment(Qt::AlignRight));
        QCOMPARE(label->font().pointSize(), QGuiApplication::primaryScreen()->geometry().width() / 90);
    }

    void chooserDuplicateLabelIdentity_data()
    {
        QTest::addColumn<int>("tab");
        QTest::addColumn<QString>("kind");
        QTest::newRow("gauge") << 0 << QString("Gauge");
        QTest::newRow("digital") << 1 << QString("Digital");
        QTest::newRow("switch") << 2 << QString("Switch");
    }

    void chooserDuplicateLabelIdentity()
    {
        QFETCH(int, tab);
        QFETCH(QString, kind);
        ModalDriver driver{QString()};
        driver.start();
        TestServices services{config_root_.path()};
        MainWindow window{services.services()};
        driver.stop();
        prepareLogging(window, "SSM");
        installLoggingFixture(
            window,
            {.parameters = {{.protocol = "OTHER", .id = "first", .name = "Same", .enabled = true},
                            {.protocol = "SSM", .id = "first", .name = "Same", .enabled = true},
                            {.protocol = "SSM", .id = "second", .name = "Same", .enabled = true}},
             .switches = {{.protocol = "OTHER", .id = "first", .name = "Same", .enabled = true},
                          {.protocol = "SSM", .id = "first", .name = "Same", .enabled = true},
                          {.protocol = "SSM", .id = "second", .name = "Same", .enabled = true}}},
            {.protocol = "SSM", .gauge_ids = {"second"}, .lower_panel_ids = {"second"}, .switch_ids = {"second"}});
        bool inspected = false;
        QTimer::singleShot(0, &window,
                           [&]
                           {
                               auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                               QVERIFY(dialog != nullptr);
                               auto *combo = dialog->findChild<QComboBox *>(kind + " value 0");
                               QVERIFY(combo != nullptr);
                               QCOMPARE(combo->count(), 2);
                               QCOMPARE(combo->currentData().toStringList(), (QStringList{"SSM", "second"}));
                               QCOMPARE(combo->itemText(0), QString("Same"));
                               QCOMPARE(combo->itemText(1), QString("Same"));
                               combo->setCurrentIndex(0);
                               inspected = true;
                               dialog->reject();
                           });
        window.change_log_values(tab, "SSM");
        QVERIFY(inspected);
        const auto& selected = window.loggerModel->selection();
        QCOMPARE((tab == 0   ? selected.gauge_ids
                  : tab == 1 ? selected.lower_panel_ids
                             : selected.switch_ids)
                     .at(0),
                 std::string("first"));
        QCOMPARE(selected.protocol, std::string("SSM"));
    }

    void csvSharedIdProtocolIdentity()
    {
        ModalDriver driver{QString()};
        driver.start();
        TestServices services{config_root_.path()};
        MainWindow window{services.services()};
        driver.stop();
        prepareLogging(window, "CDBG");
        installLoggingFixture(window,
                              {.parameters = {{.protocol = "SSM",
                                               .id = "rpm",
                                               .name = "Wrong SSM",
                                               .address = "10",
                                               .length = "1",
                                               .enabled = true,
                                               .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}},
                                              {.protocol = "CDBG",
                                               .id = "rpm",
                                               .name = "Correct CDBG",
                                               .address = "10",
                                               .length = "1",
                                               .enabled = true,
                                               .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}}}},
                              {.protocol = "CDBG",
                               .gauge_ids = {"missing"},
                               .lower_panel_ids = {"rpm", "unresolved"},
                               .switch_ids = {"missing-switch"}});
        QVERIFY(window.loggerValues.set_parameter_value({"SSM", "rpm"}, "11.00"));
        QVERIFY(window.loggerValues.set_parameter_value({"CDBG", "rpm"}, "22.00"));
        auto snapshot = fastecu::desktop::logging::make_desktop_logging_snapshot(
            *window.loggerModel, fastecu::logging::LoggingProtocolId::Cdbg, "CDBG",
            {.poll_timeout = std::chrono::milliseconds{50},
             .car_silence_miss_threshold = 20,
             .reconnect_attempt_threshold = 100,
             .reconnect_retry_period = 20});
        QVERIFY(snapshot.has_value());
        window.activeLoggingSnapshot = *snapshot;
        window.protocol = "SSM"; // active run, not mutable UI choice, owns CSV protocol
        QVERIFY(QDir().mkpath(QString::fromStdString(services.config.effective_paths().datalog_files_directory)));
        window.write_datalog_to_file = true;
        window.log_to_file();
        window.log_to_file();
        window.datalog_file_outstream.flush();
        QFile csv{window.datalog_file.fileName()};
        QVERIFY(csv.open(QIODevice::ReadOnly));
        const auto content = csv.readAll();
        QVERIFY(content.startsWith("Time,,Correct CDBG,,,\n"));
        QVERIFY(content.contains(",,22.00,,,\n"));
        QVERIFY(!content.contains("Wrong SSM"));
        QVERIFY(!content.contains("11.00"));
        window.datalog_file.close();
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
        installLoggingFixture(window,
                              {.parameters = {{.protocol = std::string("SSM"),
                                               .id = "rpm",
                                               .name = "rpm",
                                               .address = "000010",
                                               .length = "1",
                                               .ecu_byte_index = "0",
                                               .ecu_bit = "0",
                                               .target = "ECU",
                                               .enabled = true,
                                               .conversions = {{"rpm", "x", "0", "0", "100", "1"}}}}},
                              {.protocol = std::string("SSM"), .lower_panel_ids = {"rpm"}});
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
        QVERIFY(window.loggerModel->parameter_supported("SSM", "rpm"));
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
    void copyFixtureConfig(const QString& root)
    {
        const QString suffix = "/" + QString::fromStdString(kTestApplication.version) + "/config/";
        const QDir source{config_root_.path() + suffix};
        QVERIFY(QDir().mkpath(root + suffix));
        for (const QString& name : source.entryList(QDir::Files))
        {
            QVERIFY(QFile::copy(source.filePath(name), root + suffix + name));
        }
    }

    // Selects the fixture's last vehicle row using `protocol`.
    static void selectProtocol(MainWindow& window, const QString& protocol)
    {
        QVERIFY(window.configSession->select_by_protocol_name(protocol.toStdString()));
    }

    // Selects the fixture's first Subaru row using `protocol`. The flash tests
    // dispatch only for Subaru/Mitsubishi, and sub_ecu_denso_sh7058's last row
    // is a Nissan one.
    static void selectSubaruProtocol(MainWindow& window, const QString& protocol)
    {
        const auto vehicles = window.configSession->vehicles();
        const auto it = std::ranges::find_if(
            vehicles, [&](const fastecu::config::ResolvedCarModel& vehicle)
            { return vehicle.make == "Subaru" && vehicle.protocol_name == protocol.toStdString(); });
        QVERIFY(it != vehicles.end());
        QVERIFY(window.configSession->select_row(static_cast<std::size_t>(it - vehicles.begin())).has_value());
    }

    // Selects the fixture's first vehicle row of `make`.
    static void selectMake(MainWindow& window, const QString& make)
    {
        const auto vehicles = window.configSession->vehicles();
        const auto it = std::ranges::find(vehicles, make.toStdString(), &fastecu::config::ResolvedCarModel::make);
        QVERIFY(it != vehicles.end());
        QVERIFY(window.configSession->select_row(static_cast<std::size_t>(it - vehicles.begin())).has_value());
    }

    // Points the window at one open port on the given make and log transport.
    static void prepareConnect(MainWindow& window, FakeBackend& fake, const QString& make, const QString& transport)
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

    // The logging setup loggingCapturesTargetForEachRun and
    // loggingUsesTheSessionLogProtocol share: an identified ECU, a
    // "Logging" menu action, and one enabled `log_protocol` value.
    static void installLoggingFixture(MainWindow& window, fastecu::logging::LoggerDefinition definition,
                                      fastecu::logging::LoggerSelection selection)
    {
        *window.loggerModel = fastecu::logging::LoggerModel{};
        window.loggerModel->install_definition(std::move(definition));
        window.loggerModel->set_selection(std::move(selection));
        window.loggerValues.initialize(*window.loggerModel);
    }

    static QAction *prepareLogging(MainWindow& window, const QString& log_protocol)
    {
        window.vbatt_timer->stop();
        window.ecu_init_complete = true;
        window.protocol = log_protocol;
        auto *menu = window.ui->menubar->addMenu("Test logging");
        auto *action = menu->addAction("Logging");
        action->setCheckable(true);
        installLoggingFixture(window,
                              {.parameters = {{.protocol = log_protocol.toStdString(),
                                               .id = "rpm",
                                               .name = "rpm",
                                               .address = "000010",
                                               .length = "1",
                                               .ecu_byte_index = "0",
                                               .ecu_bit = "0",
                                               .target = "ECU",
                                               .enabled = true,
                                               .conversions = {{"rpm", "x", "0", "0", "100", "1"}}}}},
                              {.protocol = log_protocol.toStdString(), .lower_panel_ids = {"rpm"}});
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
