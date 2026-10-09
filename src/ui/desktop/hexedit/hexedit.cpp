#include "hexedit.h"

/*****************************************************************************/
/* Public methods */
/*****************************************************************************/
HexEdit::HexEdit(const QByteArray& data, const QString& fileName, QWidget *parent) : QMainWindow(parent)
{
    setAcceptDrops(true);
    init();
    setCurrentFile("");

    hex_edit_->setData(data);
    setCurrentFile(fileName);

    this->show();
}

/*****************************************************************************/
/* Protected methods */
/*****************************************************************************/
void HexEdit::closeEvent(QCloseEvent *event)
{
    writeSettings();

    if (is_modified_)
    {
        QMessageBox msgBox;
        msgBox.setText(tr("The file has been modified."));
        msgBox.setInformativeText(tr("Do you want to save your changes?"));
        msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Save);
        switch (msgBox.exec())
        {
        case QMessageBox::Save:
            save();
            event->accept();
            break;
        case QMessageBox::Discard:
            event->accept();
            break;
        default:
            event->ignore();
            break;
        }
    }
    else
    {
        event->accept();
    }
}

void HexEdit::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
    {
        event->accept();
    }
}

void HexEdit::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasUrls())
    {
        QList<QUrl> urls = event->mimeData()->urls();
        QString filePath = urls.at(0).toLocalFile();
        loadFile(filePath);
        event->accept();
    }
}

/*****************************************************************************/
/* Private Slots */
/*****************************************************************************/
void HexEdit::about()
{
    QMessageBox::about(this, tr("About QHexEdit"), tr("The QHexEdit example is a short Demo of the QHexEdit Widget."));
}

void HexEdit::dataChanged()
{
    is_modified_ = true;
    setWindowModified(is_modified_);
}

void HexEdit::open()
{
    QString fileName = QFileDialog::getOpenFileName(this);
    if (!fileName.isEmpty())
    {
        loadFile(fileName);
    }
}

void HexEdit::optionsAccepted()
{
    writeSettings();
    readSettings();
}

void HexEdit::findNext()
{
    search_dialog_->findNext();
}

bool HexEdit::save()
{
    if (is_untitled_)
    {
        return saveAs();
    }
    else
    {
        return saveFile(cur_file_);
    }
}

bool HexEdit::saveAs()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save As"), cur_file_);
    if (fileName.isEmpty())
    {
        return false;
    }

    return saveFile(fileName);
}

void HexEdit::saveSelectionToReadableFile()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save To Readable File"));
    if (!fileName.isEmpty())
    {
        QFile file(fileName);
        if (!file.open(QFile::WriteOnly | QFile::Text))
        {
            QMessageBox::warning(this, tr("QHexEdit"),
                                 tr("Cannot write file %1:\n%2.").arg(fileName).arg(file.errorString()));
            return;
        }

        QApplication::setOverrideCursor(Qt::WaitCursor);
        file.write(hex_edit_->selectionToReadableString().toLatin1());
        QApplication::restoreOverrideCursor();

        statusBar()->showMessage(tr("File saved"), 2000);
    }
}

void HexEdit::saveToReadableFile()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save To Readable File"));
    if (!fileName.isEmpty())
    {
        QFile file(fileName);
        if (!file.open(QFile::WriteOnly | QFile::Text))
        {
            QMessageBox::warning(this, tr("QHexEdit"),
                                 tr("Cannot write file %1:\n%2.").arg(fileName).arg(file.errorString()));
            return;
        }

        QApplication::setOverrideCursor(Qt::WaitCursor);
        file.write(hex_edit_->toReadableString().toLatin1());
        QApplication::restoreOverrideCursor();

        statusBar()->showMessage(tr("File saved"), 2000);
    }
}

void HexEdit::setAddress(qint64 address)
{
    lb_address_->setText(QString("%1").arg(address, 1, 16));
}

void HexEdit::setOverwriteMode(bool mode)
{
    QSettings settings;
    settings.setValue("OverwriteMode", mode);
    if (mode)
    {
        lb_overwrite_mode_->setText(tr("Overwrite"));
    }
    else
    {
        lb_overwrite_mode_->setText(tr("Insert"));
    }
}

void HexEdit::setSize(qint64 size)
{
    lb_size_->setText(QString("%1").arg(size));
}

void HexEdit::showOptionsDialog()
{
    options_dialog_->showWithSettings();
}

void HexEdit::showSearchDialog()
{
    search_dialog_->show();
}

/*****************************************************************************/
/* Private Methods */
/*****************************************************************************/
void HexEdit::init()
{
    setAttribute(Qt::WA_DeleteOnClose);
    options_dialog_ = new OptionsDialog(this);
    connect(options_dialog_, SIGNAL(accepted()), this, SLOT(optionsAccepted()));
    is_untitled_ = true;
    is_modified_ = false;

    hex_edit_ = new QHexEdit;
    setCentralWidget(hex_edit_);

    connect(hex_edit_, SIGNAL(overwriteModeChanged(bool)), this, SLOT(setOverwriteMode(bool)));
    connect(hex_edit_, SIGNAL(dataChanged()), this, SLOT(dataChanged()));
    search_dialog_ = new SearchDialog(hex_edit_, this);

    createActions();
    createMenus();
    createToolBars();
    createStatusBar();

    readSettings();

    // setUnifiedTitleAndToolBarOnMac(true);
    this->show();
}

void HexEdit::createActions()
{
    open_act_ = new QAction(QIcon(":/images/open.png"), tr("&Open..."), this);
    open_act_->setShortcuts(QKeySequence::Open);
    open_act_->setStatusTip(tr("Open an existing file"));
    connect(open_act_, SIGNAL(triggered()), this, SLOT(open()));

    save_act_ = new QAction(QIcon(":/images/save.png"), tr("&Save"), this);
    save_act_->setShortcuts(QKeySequence::Save);
    save_act_->setStatusTip(tr("Save the document to disk"));
    connect(save_act_, SIGNAL(triggered()), this, SLOT(save()));

    save_as_act_ = new QAction(tr("Save &As..."), this);
    save_as_act_->setShortcuts(QKeySequence::SaveAs);
    save_as_act_->setStatusTip(tr("Save the document under a new name"));
    connect(save_as_act_, SIGNAL(triggered()), this, SLOT(saveAs()));

    save_readable_ = new QAction(tr("Save &Readable..."), this);
    save_readable_->setStatusTip(tr("Save document in readable form"));
    connect(save_readable_, SIGNAL(triggered()), this, SLOT(saveToReadableFile()));

    exit_act_ = new QAction(tr("E&xit"), this);
    exit_act_->setShortcuts(QKeySequence::Quit);
    exit_act_->setStatusTip(tr("Exit the application"));
    connect(exit_act_, SIGNAL(triggered()), qApp, SLOT(closeAllWindows()));

    undo_act_ = new QAction(QIcon(":/images/undo.png"), tr("&Undo"), this);
    undo_act_->setShortcuts(QKeySequence::Undo);
    connect(undo_act_, SIGNAL(triggered()), hex_edit_, SLOT(undo()));

    redo_act_ = new QAction(QIcon(":/images/redo.png"), tr("&Redo"), this);
    redo_act_->setShortcuts(QKeySequence::Redo);
    connect(redo_act_, SIGNAL(triggered()), hex_edit_, SLOT(redo()));

    save_selection_readable_ = new QAction(tr("&Save Selection Readable..."), this);
    save_selection_readable_->setStatusTip(tr("Save selection in readable form"));
    connect(save_selection_readable_, SIGNAL(triggered()), this, SLOT(saveSelectionToReadableFile()));

    about_act_ = new QAction(tr("&About"), this);
    about_act_->setStatusTip(tr("Show the application's About box"));
    connect(about_act_, SIGNAL(triggered()), this, SLOT(about()));

    about_qt_act_ = new QAction(tr("About &Qt"), this);
    about_qt_act_->setStatusTip(tr("Show the Qt library's About box"));
    connect(about_qt_act_, SIGNAL(triggered()), qApp, SLOT(aboutQt()));

    find_act_ = new QAction(QIcon(":/images/find.png"), tr("&Find/Replace"), this);
    find_act_->setShortcuts(QKeySequence::Find);
    find_act_->setStatusTip(tr("Show the Dialog for finding and replacing"));
    connect(find_act_, SIGNAL(triggered()), this, SLOT(showSearchDialog()));

    find_next_act_ = new QAction(tr("Find &next"), this);
    find_next_act_->setShortcuts(QKeySequence::FindNext);
    find_next_act_->setStatusTip(tr("Find next occurrence of the searched pattern"));
    connect(find_next_act_, SIGNAL(triggered()), this, SLOT(findNext()));

    options_act_ = new QAction(tr("&Options"), this);
    options_act_->setStatusTip(tr("Show the Dialog to select applications options"));
    connect(options_act_, SIGNAL(triggered()), this, SLOT(showOptionsDialog()));
}

void HexEdit::createMenus()
{
    file_menu_ = menuBar()->addMenu(tr("&File"));
    file_menu_->addAction(open_act_);
    file_menu_->addAction(save_act_);
    file_menu_->addAction(save_as_act_);
    file_menu_->addAction(save_readable_);
    file_menu_->addSeparator();
    file_menu_->addAction(exit_act_);

    edit_menu_ = menuBar()->addMenu(tr("&Edit"));
    edit_menu_->addAction(undo_act_);
    edit_menu_->addAction(redo_act_);
    edit_menu_->addAction(save_selection_readable_);
    edit_menu_->addSeparator();
    edit_menu_->addAction(find_act_);
    edit_menu_->addAction(find_next_act_);
    edit_menu_->addSeparator();
    edit_menu_->addAction(options_act_);

    help_menu_ = menuBar()->addMenu(tr("&Help"));
    help_menu_->addAction(about_act_);
    help_menu_->addAction(about_qt_act_);
}

void HexEdit::createStatusBar()
{
    // Address Label
    lb_address_name_ = new QLabel();
    lb_address_name_->setText(tr("Address:"));
    statusBar()->addPermanentWidget(lb_address_name_);
    lb_address_ = new QLabel();
    lb_address_->setFrameShape(QFrame::Panel);
    lb_address_->setFrameShadow(QFrame::Sunken);
    lb_address_->setMinimumWidth(70);
    statusBar()->addPermanentWidget(lb_address_);
    connect(hex_edit_, SIGNAL(currentAddressChanged(qint64)), this, SLOT(setAddress(qint64)));

    // Size Label
    lb_size_name_ = new QLabel();
    lb_size_name_->setText(tr("Size:"));
    statusBar()->addPermanentWidget(lb_size_name_);
    lb_size_ = new QLabel();
    lb_size_->setFrameShape(QFrame::Panel);
    lb_size_->setFrameShadow(QFrame::Sunken);
    lb_size_->setMinimumWidth(70);
    statusBar()->addPermanentWidget(lb_size_);
    connect(hex_edit_, SIGNAL(currentSizeChanged(qint64)), this, SLOT(setSize(qint64)));

    // Overwrite Mode Label
    lb_overwrite_mode_name_ = new QLabel();
    lb_overwrite_mode_name_->setText(tr("Mode:"));
    statusBar()->addPermanentWidget(lb_overwrite_mode_name_);
    lb_overwrite_mode_ = new QLabel();
    lb_overwrite_mode_->setFrameShape(QFrame::Panel);
    lb_overwrite_mode_->setFrameShadow(QFrame::Sunken);
    lb_overwrite_mode_->setMinimumWidth(70);
    statusBar()->addPermanentWidget(lb_overwrite_mode_);
    setOverwriteMode(hex_edit_->overwriteMode());

    statusBar()->showMessage(tr("Ready"), 2000);
}

void HexEdit::createToolBars()
{
    file_tool_bar_ = addToolBar(tr("File"));
    file_tool_bar_->addAction(open_act_);
    file_tool_bar_->addAction(save_act_);
    edit_tool_bar_ = addToolBar(tr("Edit"));
    edit_tool_bar_->addAction(undo_act_);
    edit_tool_bar_->addAction(redo_act_);
    edit_tool_bar_->addAction(find_act_);
}

void HexEdit::loadFile(const QString& fileName)
{
    file_.setFileName(fileName);
    if (!hex_edit_->setData(file_))
    {
        QMessageBox::warning(this, tr("QHexEdit"),
                             tr("Cannot read file %1:\n%2.").arg(fileName).arg(file_.errorString()));
        return;
    }
    setCurrentFile(fileName);
    statusBar()->showMessage(tr("File loaded"), 2000);
}

void HexEdit::readSettings()
{
    // Defaults match OptionsDialog: an unpersisted store must not read as zero.
    QSettings settings;
    QPoint pos = settings.value("pos", QPoint(200, 200)).toPoint();
    QSize size = settings.value("size", QSize(610, 460)).toSize();
    move(pos);
    resize(size);

    hex_edit_->setAddressArea(settings.value("AddressArea", true).toBool());
    hex_edit_->setAsciiArea(settings.value("AsciiArea", true).toBool());
    hex_edit_->setBarArea(settings.value("BarArea", true).toBool());
    hex_edit_->setHighlighting(settings.value("Highlighting", true).toBool());
    hex_edit_->setOverwriteMode(settings.value("OverwriteMode", true).toBool());
    hex_edit_->setReadOnly(settings.value("ReadOnly").toBool());

    hex_edit_->setHighlightingColor(settings.value("HighlightingColor").value<QColor>());
    hex_edit_->setAddressAreaColor(settings.value("AddressAreaColor").value<QColor>());
    hex_edit_->setSelectionColor(settings.value("SelectionColor").value<QColor>());
    hex_edit_->setMonospaceFont(settings.value("WidgetFont").value<QFont>());
    hex_edit_->setAddressFontColor(settings.value("AddressFontColor").value<QColor>());
    hex_edit_->setAsciiAreaColor(settings.value("AsciiAreaColor").value<QColor>());
    hex_edit_->setAsciiFontColor(settings.value("AsciiFontColor").value<QColor>());
    hex_edit_->setBarAreaColor(settings.value("BarAreaColor").value<QColor>());
    hex_edit_->setBarFontColor(settings.value("BarFontColor").value<QColor>());
    hex_edit_->setHexFontColor(settings.value("HexFontColor").value<QColor>());

    hex_edit_->setAddressWidth(settings.value("AddressAreaWidth", 4).toInt());
    hex_edit_->setBytesPerLine(settings.value("BytesPerLine", 16).toInt());
    hex_edit_->setHexCaps(settings.value("HexCaps", true).toBool());
}

bool HexEdit::saveFile(const QString& fileName)
{
    QString tmpFileName = fileName + ".~tmp";

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QFile file(tmpFileName);
    bool ok = hex_edit_->write(file);
    if (QFile::exists(fileName))
    {
        ok = QFile::remove(fileName);
    }
    if (ok)
    {
        file.setFileName(tmpFileName);
        ok = file.copy(fileName);
        if (ok)
        {
            ok = QFile::remove(tmpFileName);
            is_modified_ = false;
            setWindowModified(false);
        }
    }
    QApplication::restoreOverrideCursor();

    if (!ok)
    {
        QMessageBox::warning(this, tr("QHexEdit"), tr("Cannot write file %1.").arg(fileName));
        return false;
    }

    setCurrentFile(fileName);
    statusBar()->showMessage(tr("File saved"), 2000);
    return true;
}

void HexEdit::setCurrentFile(const QString& fileName)
{
    cur_file_ = QFileInfo(fileName).canonicalFilePath();
    is_untitled_ = fileName.isEmpty();
    setWindowModified(false);
    if (fileName.isEmpty())
    {
        setWindowFilePath("QHexEdit");
    }
    else
    {
        setWindowFilePath(cur_file_ + " - QHexEdit");
    }
}

QString HexEdit::strippedName(const QString& fullFileName)
{
    return QFileInfo(fullFileName).fileName();
}

void HexEdit::writeSettings()
{
    QSettings settings;
    settings.setValue("pos", pos());
    settings.setValue("size", size());
}
