#ifndef OPTIONSDIALOG_H
#define OPTIONSDIALOG_H

#include <memory>

#include <QtCore>
#include <QDialog>

namespace Ui {
    class OptionsDialog;
}

class OptionsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit OptionsDialog(QWidget *parent = 0);
    ~OptionsDialog();
    void show();

public slots:
    virtual void accept();

private slots:
    void on_pbHighlightingColor_clicked();
    void on_pbAddressAreaColor_clicked();
    void on_pbAddressFontColor_clicked();
    void on_pbAsciiAreaColor_clicked();
    void on_pbAsciiFontColor_clicked();
    void on_pbHexFontColor_clicked();
    void on_pbSelectionColor_clicked();
    void on_pbWidgetFont_clicked();

private:
    void readSettings();
    void writeSettings();
    void setColor(QWidget *widget, QColor color);

private:
    std::unique_ptr<Ui::OptionsDialog> ui;
};

#endif // OPTIONSDIALOG_H
