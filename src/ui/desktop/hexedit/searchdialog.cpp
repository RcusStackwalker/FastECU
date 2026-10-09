#include "searchdialog.h"
#include "ui_searchdialog.h"

#include <QMessageBox>

SearchDialog::SearchDialog(QHexEdit *hexEdit, QWidget *parent)
    : QDialog(parent), ui_{std::make_unique<Ui::SearchDialog>()}
{
    ui_->setupUi(this);
    hex_edit_ = hexEdit;
}

SearchDialog::~SearchDialog()
{
}

qint64 SearchDialog::findNext()
{
    qint64 from = hex_edit_->cursorPosition() / 2;
    find_ba_ = getContent(ui_->cbFindFormat->currentIndex(), ui_->cbFind->currentText());
    qint64 idx = -1;

    if (find_ba_.length() > 0)
    {
        if (ui_->cbBackwards->isChecked())
        {
            idx = hex_edit_->lastIndexOf(find_ba_, from);
        }
        else
        {
            idx = hex_edit_->indexOf(find_ba_, from);
        }
    }
    return idx;
}

void SearchDialog::on_pbFind_clicked()
{
    findNext();
}

void SearchDialog::on_pbReplace_clicked()
{
    qint64 idx = findNext();
    if (idx >= 0)
    {
        QByteArray replaceBa = getContent(ui_->cbReplaceFormat->currentIndex(), ui_->cbReplace->currentText());
        replaceOccurrence(idx, replaceBa);
    }
}

void SearchDialog::on_pbReplaceAll_clicked()
{
    int replaceCounter = 0;
    qint64 idx = 0;
    int goOn = QMessageBox::Yes;

    while ((idx >= 0) && (goOn == QMessageBox::Yes))
    {
        idx = findNext();
        if (idx >= 0)
        {
            QByteArray replaceBa = getContent(ui_->cbReplaceFormat->currentIndex(), ui_->cbReplace->currentText());
            int result = static_cast<int>(replaceOccurrence(idx, replaceBa));

            if (result == QMessageBox::Yes)
            {
                replaceCounter += 1;
            }

            if (result == QMessageBox::Cancel)
            {
                goOn = result;
            }
        }
    }

    if (replaceCounter > 0)
    {
        QMessageBox::information(this, tr("QHexEdit"), QString(tr("%1 occurrences replaced.")).arg(replaceCounter));
    }
}

QByteArray SearchDialog::getContent(int comboIndex, const QString& input)
{
    QByteArray findBa;
    switch (comboIndex)
    {
    case 0: // hex
        findBa = QByteArray::fromHex(input.toLatin1());
        break;
    case 1: // text
        findBa = input.toUtf8();
        break;
    default:
        break;
    }
    return findBa;
}

qint64 SearchDialog::replaceOccurrence(qint64 idx, const QByteArray& replaceBa)
{
    int result = QMessageBox::Yes;
    if (replaceBa.length() >= 0)
    {
        if (ui_->cbPrompt->isChecked())
        {
            result = QMessageBox::question(this, tr("QHexEdit"), tr("Replace occurrence?"),
                                           QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

            if (result == QMessageBox::Yes)
            {
                hex_edit_->replace(idx, replaceBa.length(), replaceBa);
                hex_edit_->update();
            }
        }
        else
        {
            hex_edit_->replace(idx, find_ba_.length(), replaceBa);
        }
    }
    return result;
}
