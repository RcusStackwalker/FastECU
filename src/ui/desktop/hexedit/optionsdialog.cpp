
#include <QColorDialog>
#include <QFontDialog>

#include "optionsdialog.h"
#include "ui_optionsdialog.h"

OptionsDialog::OptionsDialog(QWidget *parent) : QDialog(parent), ui_{std::make_unique<Ui::OptionsDialog>()}
{
    ui_->setupUi(this);
    readSettings();
    writeSettings();
}

OptionsDialog::~OptionsDialog()
{
}

void OptionsDialog::showWithSettings()
{
    readSettings();
    QWidget::show();
}

void OptionsDialog::accept()
{
    writeSettings();
    emit accepted();
    QDialog::hide();
}

void OptionsDialog::readSettings()
{
    QSettings settings;

    ui_->cbAddressArea->setChecked(settings.value("AddressArea", true).toBool());
    ui_->cbAsciiArea->setChecked(settings.value("AsciiArea", true).toBool());
    ui_->cbBarArea->setChecked(settings.value("BarArea", true).toBool());
    ui_->cbHighlighting->setChecked(settings.value("Highlighting", true).toBool());
    ui_->cbOverwriteMode->setChecked(settings.value("OverwriteMode", true).toBool());
    ui_->cbReadOnly->setChecked(settings.value("ReadOnly").toBool());

    setColor(ui_->lbHighlightingColor,
             settings.value("HighlightingColor", QColor(0xff, 0xff, 0x99, 0xff)).value<QColor>());
    setColor(ui_->lbAddressAreaColor,
             settings.value("AddressAreaColor", this->palette().alternateBase().color()).value<QColor>());
    setColor(ui_->lbSelectionColor,
             settings.value("SelectionColor", this->palette().highlight().color()).value<QColor>());
    setColor(ui_->lbAddressFontColor, settings.value("AddressFontColor", QPalette::WindowText).value<QColor>());
    setColor(ui_->lbAsciiAreaColor,
             settings.value("AsciiAreaColor", this->palette().alternateBase().color()).value<QColor>());
    setColor(ui_->lbAsciiFontColor, settings.value("AsciiFontColor", QPalette::WindowText).value<QColor>());
    setColor(ui_->lbHexFontColor, settings.value("HexFontColor", QPalette::WindowText).value<QColor>());
#ifdef Q_OS_WIN32
    ui->leWidgetFont->setFont(settings.value("WidgetFont", QFont("Courier", 10)).value<QFont>());
#else
    ui_->leWidgetFont->setFont(settings.value("WidgetFont", QFont("Monospace", 10)).value<QFont>());
#endif

    ui_->sbAddressAreaWidth->setValue(settings.value("AddressAreaWidth", 4).toInt());
    ui_->sbBytesPerLine->setValue(settings.value("BytesPerLine", 16).toInt());
}

void OptionsDialog::writeSettings()
{
    QSettings settings;
    settings.setValue("AddressArea", ui_->cbAddressArea->isChecked());
    settings.setValue("AsciiArea", ui_->cbAsciiArea->isChecked());
    settings.setValue("BarArea", ui_->cbBarArea->isChecked());
    settings.setValue("Highlighting", ui_->cbHighlighting->isChecked());
    settings.setValue("OverwriteMode", ui_->cbOverwriteMode->isChecked());
    settings.setValue("ReadOnly", ui_->cbReadOnly->isChecked());

    settings.setValue("HighlightingColor", ui_->lbHighlightingColor->palette().color(QPalette::Window));
    settings.setValue("AddressAreaColor", ui_->lbAddressAreaColor->palette().color(QPalette::Window));
    settings.setValue("SelectionColor", ui_->lbSelectionColor->palette().color(QPalette::Window));
    settings.setValue("AddressFontColor", ui_->lbAddressFontColor->palette().color(QPalette::Window));
    settings.setValue("AsciiAreaColor", ui_->lbAsciiAreaColor->palette().color(QPalette::Window));
    settings.setValue("AsciiFontColor", ui_->lbAsciiFontColor->palette().color(QPalette::Window));
    settings.setValue("HexFontColor", ui_->lbHexFontColor->palette().color(QPalette::Window));
    settings.setValue("WidgetFont", ui_->leWidgetFont->font());

    settings.setValue("AddressAreaWidth", ui_->sbAddressAreaWidth->value());
    settings.setValue("BytesPerLine", ui_->sbBytesPerLine->value());
}

void OptionsDialog::setColor(QWidget *widget, QColor color)
{
    QPalette palette = widget->palette();
    palette.setColor(QPalette::Window, color);
    widget->setPalette(palette);
    widget->setAutoFillBackground(true);
}

void OptionsDialog::on_pbHighlightingColor_clicked()
{
    QColor color = QColorDialog::getColor(ui_->lbHighlightingColor->palette().color(QPalette::Window), this);
    if (color.isValid())
    {
        setColor(ui_->lbHighlightingColor, color);
    }
}

void OptionsDialog::on_pbAddressAreaColor_clicked()
{
    QColor color = QColorDialog::getColor(ui_->lbAddressAreaColor->palette().color(QPalette::Window), this);
    if (color.isValid())
    {
        setColor(ui_->lbAddressAreaColor, color);
    }
}

void OptionsDialog::on_pbAddressFontColor_clicked()
{
    QColor color = QColorDialog::getColor(ui_->lbAddressFontColor->palette().color(QPalette::WindowText), this);
    if (color.isValid())
    {
        setColor(ui_->lbAddressFontColor, color);
    }
}

void OptionsDialog::on_pbAsciiAreaColor_clicked()
{
    QColor color = QColorDialog::getColor(ui_->lbAsciiAreaColor->palette().color(QPalette::Window), this);
    if (color.isValid())
    {
        setColor(ui_->lbAsciiAreaColor, color);
    }
}

void OptionsDialog::on_pbAsciiFontColor_clicked()
{
    QColor color = QColorDialog::getColor(ui_->lbAsciiFontColor->palette().color(QPalette::WindowText), this);
    if (color.isValid())
    {
        setColor(ui_->lbAsciiFontColor, color);
    }
}

void OptionsDialog::on_pbHexFontColor_clicked()
{
    QColor color = QColorDialog::getColor(ui_->lbHexFontColor->palette().color(QPalette::WindowText), this);
    if (color.isValid())
    {
        setColor(ui_->lbHexFontColor, color);
    }
}

void OptionsDialog::on_pbSelectionColor_clicked()
{
    QColor color = QColorDialog::getColor(ui_->lbSelectionColor->palette().color(QPalette::Window), this);
    if (color.isValid())
    {
        setColor(ui_->lbSelectionColor, color);
    }
}

void OptionsDialog::on_pbWidgetFont_clicked()
{
    bool ok;
    QFont font = QFontDialog::getFont(&ok, ui_->leWidgetFont->font(), this);
    if (ok)
    {
        ui_->leWidgetFont->setFont(font);
    }
}
