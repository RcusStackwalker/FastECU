#ifndef SEARCHDIALOG_H
#define SEARCHDIALOG_H

#include <memory>

#include <QDialog>
#include <QtCore>
#include "./qhexedit/qhexedit.h"

namespace Ui
{
class SearchDialog;
}

class SearchDialog : public QDialog
{
    Q_OBJECT
  public:
    explicit SearchDialog(QHexEdit *hexEdit, QWidget *parent = 0);
    ~SearchDialog();
    qint64 findNext();

  private slots:
    void on_pbFind_clicked();
    void on_pbReplace_clicked();
    void on_pbReplaceAll_clicked();

  private:
    QByteArray getContent(int comboIndex, const QString& input);
    qint64 replaceOccurrence(qint64 idx, const QByteArray& replaceBa);

    QHexEdit *_hexEdit;
    QByteArray _findBa;

  private:
    std::unique_ptr<Ui::SearchDialog> ui;
};

#endif // SEARCHDIALOG_H
