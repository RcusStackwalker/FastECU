#include "src/ui/desktop/widgets/settings.h"
#include "ui_settings.h"

#include <memory>
#include <tuple>

#include <QFileDialog>
#include <QMessageBox>

#include "src/ui/desktop/config_fields.h"

using fastecu::ui::qs;

Settings::Settings(fastecu::config::ConfigSession& config, QWidget *parent)
    : QDialog(parent), config_(config), ui_{std::make_unique<Ui::Settings>()}
{
    ui_->setupUi(this);

    ui_->list_widget->setViewMode(QListView::IconMode);
    // ui->list_widget->setIconSize(QSize(96, 84));
    ui_->list_widget->setMovement(QListView::Static);
    ui_->list_widget->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    // ui->list_widget->setFixedWidth(160);
    ui_->list_widget->setSpacing(10);

    ui_->dir_page->setLayout(create_files_config_page());
    ui_->ui_page->setLayout(create_ui_config_page());

    ui_->save_button->hide();
    // connect(ui->save_button, SIGNAL (clicked()), this, SLOT (save_config_file()));

    connect(ui_->close_button, SIGNAL(clicked()), this, SLOT(close()));

    create_list_icons();
    ui_->list_widget->setCurrentRow(0);

    QHBoxLayout *buttonsLayout = new QHBoxLayout;
    buttonsLayout->addStretch(1);
}

Settings::~Settings()
{
    qDebug() << "Save config file before exit, bye bye!";
    if (close_save_attempted_)
    {
        // Retry persistence without repeating the notification shown on close.
        std::ignore = config_.save();
    }
    else
    {
        save_config_file();
    }
}

void Settings::closeEvent(QCloseEvent *bar)
{
    qDebug() << "Save config file before exit, bye bye!";
    save_config_file();
    close_save_attempted_ = true;
}

int Settings::save_config_file()
{
    if (const fastecu::Status saved = config_.save(); !saved.has_value())
    {
        // The edits stay in the session; the operator learns why they did
        // not reach the file.
        QMessageBox::warning(this, tr("Settings"),
                             tr("Unable to save settings.\n\n%1").arg(QString::fromStdString(saved.error().detail)));
        return 1;
    }
    return 0;
}

QVBoxLayout *Settings::create_files_config_page()
{
    QGroupBox *romraiderDefinitionsGroup = new QGroupBox(tr("RomRaider Definition Files"));
    /*
    qDebug() << "Calibration path:" << qs(config.settings().calibration_files_directory);
    qDebug() << "Romraider definition files:" <<
    fastecu::ui::qstring_list(config.settings().romraider_definition_files); qDebug() << "EcuFlash definition path:" <<
    qs(config.settings().ecuflash_definition_files_directory); qDebug() << "Log files path:" <<
    qs(config.settings().datalog_files_directory);
*/

    QCheckBox *romraiderDefsEnabled = new QCheckBox("Enabled");
    if (config_.settings().use_romraider_definitions == "enabled")
    {
        romraiderDefsEnabled->setChecked(true);
    }
    connect(romraiderDefsEnabled, SIGNAL(stateChanged(int)), this, SLOT(romraider_defs_enabled_checkbox(int)));

    romraider_definition_files_list_ = new QListWidget;

    for (const std::string& file : config_.settings().romraider_definition_files)
    {
        new QListWidgetItem(qs(file), romraider_definition_files_list_);
    }

    QCheckBox *romraiderAsPrimaryDefBase = new QCheckBox("Use as primary");
    if (config_.settings().primary_definition_base == "romraider")
    {
        romraiderAsPrimaryDefBase->setChecked(true);
    }
    connect(romraiderAsPrimaryDefBase, SIGNAL(stateChanged(int)), this,
            SLOT(romraider_as_primary_def_base_checkbox(int)));

    QPushButton *romraiderDefFilesAddButton = new QPushButton;
    romraiderDefFilesAddButton->setFixedWidth(100);
    romraiderDefFilesAddButton->setText("Add");
    connect(romraiderDefFilesAddButton, &QAbstractButton::clicked, this, &Settings::add_definition_files);

    QPushButton *romraiderDefFilesRemoveButton = new QPushButton;
    romraiderDefFilesRemoveButton->setFixedWidth(100);
    romraiderDefFilesRemoveButton->setText("Remove");
    connect(romraiderDefFilesRemoveButton, &QAbstractButton::clicked, this, &Settings::remove_definition_files);

    QHBoxLayout *romraiderDefFilesButtonsLayout = new QHBoxLayout;
    romraiderDefFilesButtonsLayout->addWidget(romraiderAsPrimaryDefBase);
    romraiderDefFilesButtonsLayout->addStretch(1);
    // romraider_def_files_buttons_layout->setAlignment(Qt::AlignRight);
    romraiderDefFilesButtonsLayout->addWidget(romraiderDefFilesAddButton);
    romraiderDefFilesButtonsLayout->addWidget(romraiderDefFilesRemoveButton);

    QVBoxLayout *romraiderDefFilesLayout = new QVBoxLayout;
    romraiderDefFilesLayout->addWidget(romraiderDefsEnabled);
    romraiderDefFilesLayout->addWidget(romraider_definition_files_list_);
    romraiderDefFilesLayout->addLayout(romraiderDefFilesButtonsLayout);

    QCheckBox *ecuflashDefsEnabled = new QCheckBox("Enabled");
    if (config_.settings().use_ecuflash_definitions == "enabled")
    {
        ecuflashDefsEnabled->setChecked(true);
    }
    connect(ecuflashDefsEnabled, SIGNAL(stateChanged(int)), this, SLOT(ecuflash_defs_enabled_checkbox(int)));

    QGroupBox *ecuflashDefDirGroup = new QGroupBox(tr("EcuFlash Definition Files Directory"));
    ecuflash_def_dir_lineedit_ = new QLineEdit;
    ecuflash_def_dir_lineedit_->setText(qs(config_.settings().ecuflash_definition_files_directory));

    QPushButton *ecuflashDefDirBrowseButton = new QPushButton;
    ecuflashDefDirBrowseButton->setIcon(QIcon(":/icons/document-open.png"));
    connect(ecuflashDefDirBrowseButton, &QAbstractButton::clicked, this, &Settings::set_ecuflash_def_dir);

    QHBoxLayout *ecuflashDefDirLayout = new QHBoxLayout;
    ecuflashDefDirLayout->addWidget(ecuflash_def_dir_lineedit_);
    ecuflashDefDirLayout->addWidget(ecuflashDefDirBrowseButton);

    QGroupBox *romraiderLoggerFileGroup = new QGroupBox(tr("RomRaider Logger File"));
    romraider_logger_file_lineedit_ = new QLineEdit;
    romraider_logger_file_lineedit_->setText(qs(config_.settings().romraider_logger_definition_file));

    QPushButton *romraiderLoggerFileBrowseButton = new QPushButton;
    romraiderLoggerFileBrowseButton->setIcon(QIcon(":/icons/document-open.png"));
    connect(romraiderLoggerFileBrowseButton, &QAbstractButton::clicked, this, &Settings::set_romraider_logger_file);

    QHBoxLayout *romraiderLoggerFileLayout = new QHBoxLayout;
    romraiderLoggerFileLayout->addWidget(romraider_logger_file_lineedit_);
    romraiderLoggerFileLayout->addWidget(romraiderLoggerFileBrowseButton);

    QGroupBox *ecuCalDirGroup = new QGroupBox(tr("Calibrations Directory"));
    ecu_cal_dir_lineedit_ = new QLineEdit;
    ecu_cal_dir_lineedit_->setText(qs(config_.settings().calibration_files_directory));

    QPushButton *ecuCalDirBrowseButton = new QPushButton;
    ecuCalDirBrowseButton->setIcon(QIcon(":/icons/document-open.png"));
    connect(ecuCalDirBrowseButton, &QAbstractButton::clicked, this, &Settings::set_ecu_cal_dir);

    QHBoxLayout *ecuCalDirLayout = new QHBoxLayout;
    ecuCalDirLayout->addWidget(ecu_cal_dir_lineedit_);
    ecuCalDirLayout->addWidget(ecuCalDirBrowseButton);

    QGroupBox *logFilesGroup = new QGroupBox(tr("Logfiles Directory"));

    log_files_dir_lineedit_ = new QLineEdit;
    log_files_dir_lineedit_->setText(qs(config_.settings().datalog_files_directory));

    QPushButton *logFilesDirBrowseButton = new QPushButton;
    logFilesDirBrowseButton->setIcon(QIcon(":/icons/document-open.png"));
    connect(logFilesDirBrowseButton, &QAbstractButton::clicked, this, &Settings::set_log_files_dir);

    QHBoxLayout *logFilesDirLayout = new QHBoxLayout;
    logFilesDirLayout->addWidget(log_files_dir_lineedit_);
    logFilesDirLayout->addWidget(logFilesDirBrowseButton);

    QVBoxLayout *definitionLayout = new QVBoxLayout;
    definitionLayout->addLayout(romraiderDefFilesLayout);
    romraiderDefinitionsGroup->setLayout(definitionLayout);

    QVBoxLayout *ecuflashDefLayout = new QVBoxLayout;
    ecuflashDefLayout->addWidget(ecuflashDefsEnabled);
    ecuflashDefLayout->addLayout(ecuflashDefDirLayout);
    ecuflashDefDirGroup->setLayout(ecuflashDefLayout);

    QVBoxLayout *romraiderLoggerLayout = new QVBoxLayout;
    romraiderLoggerLayout->addLayout(romraiderLoggerFileLayout);
    romraiderLoggerFileGroup->setLayout(romraiderLoggerLayout);

    QVBoxLayout *calibrationLayout = new QVBoxLayout;
    calibrationLayout->addLayout(ecuCalDirLayout);
    ecuCalDirGroup->setLayout(calibrationLayout);

    QVBoxLayout *logFilesLayout = new QVBoxLayout;
    logFilesLayout->addLayout(logFilesDirLayout);
    logFilesGroup->setLayout(logFilesLayout);

    QVBoxLayout *directoryLayout = new QVBoxLayout;
    directoryLayout->addWidget(romraiderDefinitionsGroup);
    directoryLayout->addWidget(romraiderLoggerFileGroup);
    directoryLayout->addWidget(ecuflashDefDirGroup);
    directoryLayout->addWidget(ecuCalDirGroup);
    directoryLayout->addWidget(logFilesGroup);
    directoryLayout->addStretch(1);

    return directoryLayout;
}

QVBoxLayout *Settings::create_ui_config_page()
{
    QGroupBox *toolbarGroup = new QGroupBox(tr("Toolbar settings"));
    QLabel *toolbarIconsizeLabel = new QLabel("Toolbar icon size:");
    toolbar_iconsize_spinbox_ = new QSpinBox();
    toolbar_iconsize_spinbox_->setValue(qs(config_.settings().toolbar_iconsize).toInt());
    connect(toolbar_iconsize_spinbox_, SIGNAL(valueChanged(int)), this, SLOT(toolbar_iconsize_value_changed(int)));

    QHBoxLayout *toolbarLayout = new QHBoxLayout;
    toolbarLayout->setAlignment(Qt::AlignLeft);
    toolbarLayout->addWidget(toolbarIconsizeLabel);
    toolbarLayout->addWidget(toolbar_iconsize_spinbox_);
    toolbarGroup->setLayout(toolbarLayout);

    QVBoxLayout *uiConfigLayout = new QVBoxLayout;
    uiConfigLayout->addWidget(toolbarGroup);
    uiConfigLayout->addStretch(1);

    return uiConfigLayout;
}

void Settings::create_list_icons()
{
    QListWidgetItem *fileConfigItem = new QListWidgetItem(ui_->list_widget);
    fileConfigItem->setIcon(QIcon(":/icons/document-open.png"));
    fileConfigItem->setText(tr("Files"));
    fileConfigItem->setSizeHint(QSize(64, 64));
    // fileConfigButton->setTextAlignment(Qt::AlignHCenter);
    fileConfigItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

    QListWidgetItem *uiConfigItem = new QListWidgetItem(ui_->list_widget);
    uiConfigItem->setIcon(QIcon(":/icons/preferences-system.png"));
    uiConfigItem->setText(tr("Ui config"));
    uiConfigItem->setSizeHint(QSize(64, 64));
    // uiConfigButton->setTextAlignment(Qt::AlignHCenter);
    uiConfigItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

    connect(ui_->list_widget, &QListWidget::currentItemChanged, this, &Settings::change_page);
}

void Settings::change_page(QListWidgetItem *current, QListWidgetItem *previous)
{
    if (!current)
    {
        current = previous;
    }

    ui_->pages_widget->setCurrentIndex(ui_->list_widget->row(current));
}

void Settings::romraider_defs_enabled_checkbox(int state)
{
    if (state)
    {
        config_.settings().use_romraider_definitions = "enabled";
    }
    else
    {
        config_.settings().use_romraider_definitions = "disabled";
    }
}

void Settings::ecuflash_defs_enabled_checkbox(int state)
{
    if (state)
    {
        config_.settings().use_ecuflash_definitions = "enabled";
    }
    else
    {
        config_.settings().use_ecuflash_definitions = "disabled";
    }
}

void Settings::romraider_as_primary_def_base_checkbox(int state)
{
    if (state)
    {
        config_.settings().primary_definition_base = "romraider";
    }
    else
    {
        config_.settings().primary_definition_base = "ecuflash";
    }
}

void Settings::toolbar_iconsize_value_changed(int value)
{
    config_.settings().toolbar_iconsize = QString::number(value).toStdString();
}

void Settings::set_ecuflash_def_dir()
{

    QString ecuflashDefinitionDir = QFileDialog::getExistingDirectory(
        this, tr("Select EcuFlash definition directory"), qs(config_.settings().ecuflash_definition_files_directory),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    qDebug() << "Selected path:" << ecuflashDefinitionDir;
    // if (!ecuflash_definition_dir.endsWith("/") && !ecuflash_definition_dir.endsWith("\\"))
    //     ecuflash_definition_dir.append("/");
    if (!ecuflashDefinitionDir.isEmpty())
    {
        config_.settings().ecuflash_definition_files_directory = ecuflashDefinitionDir.toStdString();
        ecuflash_def_dir_lineedit_->clear();
        ecuflash_def_dir_lineedit_->setText(qs(config_.settings().ecuflash_definition_files_directory));
    }
    // else
    //     QMessageBox::information(this, tr("EcuFlash definition directory"), "No directory selected");
}

void Settings::set_romraider_logger_file()
{
    QString fileDir;

    QDir dir;
    if (!config_.settings().romraider_logger_definition_file.empty())
    {
        QFileInfo defFileName(qs(config_.settings().romraider_logger_definition_file));
        QString defFileDir = defFileName.absoluteFilePath();
        fileDir.append(defFileDir);
    }
    else
    {
        fileDir.append("./");
    }
    QString filename = QFileDialog::getOpenFileName(this, tr("Add RomRaider logger file"), fileDir,
                                                    tr("RomRaider logger file (*.xml)"));
    qDebug() << "Filename:" << filename;
    if (!filename.isEmpty())
    {
        config_.settings().romraider_logger_definition_file = filename.toStdString();
        romraider_logger_file_lineedit_->clear();
        romraider_logger_file_lineedit_->setText(qs(config_.settings().romraider_logger_definition_file));
    }
    // else
    //     QMessageBox::information(this, tr("RomRaider logger file"), "No logger file selected");
}

void Settings::set_ecu_cal_dir()
{

    QString calibrationDir = QFileDialog::getExistingDirectory(
        this, tr("Select calibrations directory"), qs(config_.settings().calibration_files_directory),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    qDebug() << "Selected path:" << calibrationDir;
    // if (!calibration_dir.endsWith("/") && !calibration_dir.endsWith("\\"))
    //     calibration_dir.append("/");
    if (!calibrationDir.isEmpty())
    {
        config_.settings().calibration_files_directory = calibrationDir.toStdString();
        ecu_cal_dir_lineedit_->clear();
        ecu_cal_dir_lineedit_->setText(qs(config_.settings().calibration_files_directory));
    }
    // else
    //     QMessageBox::information(this, tr("Calibration directory"), "No directory selected");
}

void Settings::set_log_files_dir()
{

    QString logfilesDir = QFileDialog::getExistingDirectory(
        this, tr("Select logfiles directory"), qs(config_.settings().datalog_files_directory),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    qDebug() << "Selected path:" << logfilesDir;
    // if (!logfiles_dir.endsWith("/") && !logfiles_dir.endsWith("\\"))
    //     logfiles_dir.append("/");
    if (!logfilesDir.isEmpty())
    {
        config_.settings().datalog_files_directory = logfilesDir.toStdString();
        log_files_dir_lineedit_->clear();
        log_files_dir_lineedit_->setText(qs(config_.settings().datalog_files_directory));
    }
    // else
    //     QMessageBox::information(this, tr("Logger files directory"), "No directory selected");
}

void Settings::add_definition_files()
{
    QString fileDir;

    QDir dir;
    if (!config_.settings().romraider_definition_files.empty())
    {
        QFileInfo defFileName(qs(config_.settings().romraider_definition_files.back()));
        QString defFileDir = defFileName.absoluteFilePath();
        fileDir.append(defFileDir);
    }
    else
    {
        fileDir.append("./");
    }
    QString filename = QFileDialog::getOpenFileName(this, tr("Add RomRaider definition file"), fileDir,
                                                    tr("RomRaider definition file (*.xml)"));

    if (!filename.isEmpty())
    {
        romraider_definition_files_list_->addItem(filename);
        romraider_definition_files_list_->update();
        config_.settings().romraider_definition_files.push_back(filename.toStdString());
    }
    // else
    //     QMessageBox::information(this, tr("RomRaider definition file"), "No definition file selected");
}

void Settings::remove_definition_files()
{

    QList<QListWidgetItem *> items = romraider_definition_files_list_->selectedItems();
    foreach (QListWidgetItem *item, items)
    {
        delete romraider_definition_files_list_->takeItem(romraider_definition_files_list_->row(item));
    }

    config_.settings().romraider_definition_files.clear();

    for (int i = 0; i < romraider_definition_files_list_->count(); ++i)
    {
        config_.settings().romraider_definition_files.push_back(
            romraider_definition_files_list_->item(i)->text().toStdString());
    }
}
