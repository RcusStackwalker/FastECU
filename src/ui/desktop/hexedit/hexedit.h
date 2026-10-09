#pragma once

#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QStatusBar>
#include <QLabel>
#include <QAction>
#include <QMenuBar>
#include <QToolBar>
#include <QColorDialog>
#include <QFontDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QVBoxLayout>

#include "./qhexedit/qhexedit.h"
#include "optionsdialog.h"
#include <QMainWindow>
#include "searchdialog.h"
#include <QByteArray>
#include <QString>

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
class QUndoStack;
class QLabel;
class QDragEnterEvent;
class QDropEvent;
QT_END_NAMESPACE

class HexEdit : public QMainWindow
{
    Q_OBJECT

  public:
    HexEdit(const QByteArray& data, const QString& file_name, QWidget *parent = nullptr);

  protected:
    void closeEvent(QCloseEvent *event);
    void dragEnterEvent(QDragEnterEvent *event);
    void dropEvent(QDropEvent *event);

  private slots:
    void about();
    void dataChanged();
    void open();
    void optionsAccepted();
    void findNext();
    bool save();
    bool saveAs();
    void saveSelectionToReadableFile();
    void saveToReadableFile();
    void setAddress(qint64 address);
    void setOverwriteMode(bool mode);
    void setSize(qint64 size);
    void showOptionsDialog();
    void showSearchDialog();

  public:
    void loadFile(const QString& fileName);

  private:
    void init();
    void createActions();
    void createMenus();
    void createStatusBar();
    void createToolBars();
    void readSettings();
    bool saveFile(const QString& fileName);
    void setCurrentFile(const QString& fileName);
    QString strippedName(const QString& fullFileName);
    void writeSettings();

    QString cur_file_;
    QFile file_;
    bool is_untitled_{};
    bool is_modified_{};

    QMenu *file_menu_{};
    QMenu *edit_menu_{};
    QMenu *help_menu_{};

    QToolBar *file_tool_bar_{};
    QToolBar *edit_tool_bar_{};

    QAction *open_act_{};
    QAction *save_act_{};
    QAction *save_as_act_{};
    QAction *save_readable_{};
    QAction *close_act_{};
    QAction *exit_act_{};

    QAction *undo_act_{};
    QAction *redo_act_{};
    QAction *save_selection_readable_{};

    QAction *about_act_{};
    QAction *about_qt_act_{};
    QAction *options_act_{};
    QAction *find_act_{};
    QAction *find_next_act_{};

    QHexEdit *hex_edit_{};
    OptionsDialog *options_dialog_{};
    SearchDialog *search_dialog_{};
    QLabel *lb_address_{}, *lb_address_name_{};
    QLabel *lb_overwrite_mode_{}, *lb_overwrite_mode_name_{};
    QLabel *lb_size_{}, *lb_size_name_{};
};
