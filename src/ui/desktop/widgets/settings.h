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

    QVBoxLayout *create_files_config_page();
    QVBoxLayout *create_ui_config_page();
    void create_list_icons();
    void change_page(QListWidgetItem *current, QListWidgetItem *previous);
    void set_ecuflash_def_dir();
    void set_romraider_logger_file();
    void set_ecu_cal_dir();
    void set_log_files_dir();
    void add_definition_files();
    void remove_definition_files();

  private slots:
    void ecuflash_defs_enabled_checkbox(int state);
    void romraider_defs_enabled_checkbox(int state);
    void romraider_as_primary_def_base_checkbox(int state);
    void toolbar_iconsize_value_changed(int value);
    int save_config_file();

  private:
    std::unique_ptr<Ui::Settings> ui_;
};
