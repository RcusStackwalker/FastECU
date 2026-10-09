#pragma once

#include <memory>

#include <QCheckBox>
#include <QDebug>
#include <QDialog>
#include <QMainWindow>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include "src/backend/config/config_session.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
class Settings;
}
QT_END_NAMESPACE

class Settings : public QDialog
{
    Q_OBJECT

  public:
    // Edits `config`'s settings live and saves them on close (and, as a
    // fallback, on destruction); a failed save is reported to the operator.
    explicit Settings(fastecu::config::ConfigSession& config, QWidget *parent = nullptr);
    ~Settings();

  private slots:

  private:
    void closeEvent(QCloseEvent *bar);

    fastecu::config::ConfigSession& config_;
    bool close_save_attempted_ = false;

    QLineEdit *ecuflash_def_dir_lineedit_{};
    QLineEdit *romraider_logger_file_lineedit_{};
    QLineEdit *ecu_cal_dir_lineedit_{};
    QLineEdit *log_files_dir_lineedit_{};

    QSpinBox *toolbar_iconsize_spinbox_{};

    QListWidget *romraider_definition_files_list_{};

    QVBoxLayout *createFilesConfigPage();
    QVBoxLayout *createUiConfigPage();
    void createListIcons();
    void changePage(QListWidgetItem *current, QListWidgetItem *previous);
    void setEcuflashDefDir();
    void setRomraiderLoggerFile();
    void setEcuCalDir();
    void setLogFilesDir();
    void addDefinitionFiles();
    void removeDefinitionFiles();

  private slots:
    void ecuflashDefsEnabledCheckbox(int state);
    void romraiderDefsEnabledCheckbox(int state);
    void romraiderAsPrimaryDefBaseCheckbox(int state);
    void toolbarIconsizeValueChanged(int value);
    int saveConfigFile();

  private:
    std::unique_ptr<Ui::Settings> ui_;
};
