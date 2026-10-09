#include "src/ui/desktop/widgets/mainwindow.h"

#include <algorithm>
#include <QDialogButtonBox>

#include "src/ui/desktop/config_fields.h"

namespace
{
// Each combo item owns an identity; presentation labels need not be unique.
struct LoggerChoice
{
    QString name;
    QStringList identity;
};
void populateChoices(QComboBox& combo, std::vector<LoggerChoice> choices, const QString& protocol,
                     const std::string& selected)
{
    std::stable_sort(choices.begin(), choices.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    for (const auto& choice : choices)
    {
        combo.addItem(choice.name, choice.identity);
    }
    combo.setCurrentIndex(combo.findData(QStringList{protocol, QString::fromStdString(selected)}));
}
void populateParameterChoices(QComboBox& combo, const fastecu::logging::LoggerModel& model, const QString& protocol,
                              const std::string& selected)
{
    std::vector<LoggerChoice> choices;
    const auto key = protocol.toStdString();
    for (const auto& p : model.Definition().parameters)
    {
        if (p.protocol == key && model.ParameterSupported(key, p.id))
        {
            choices.push_back({QString::fromStdString(p.name), {protocol, QString::fromStdString(p.id)}});
        }
    }
    populateChoices(combo, std::move(choices), protocol, selected);
}
void populateSwitchChoices(QComboBox& combo, const fastecu::logging::LoggerModel& model, const QString& protocol,
                           const std::string& selected)
{
    std::vector<LoggerChoice> choices;
    const auto key = protocol.toStdString();
    for (const auto& p : model.Definition().switches)
    {
        if (p.protocol == key && model.SwitchSupported(key, p.id))
        {
            choices.push_back({QString::fromStdString(p.name), {protocol, QString::fromStdString(p.id)}});
        }
    }
    populateChoices(combo, std::move(choices), protocol, selected);
}
bool applyChoice(fastecu::logging::LoggerSelection& selection, std::vector<std::string>& ids, const QComboBox *combo,
                 int index)
{
    if (combo == nullptr || index < 0)
    {
        return false;
    }
    const auto identity = combo->itemData(index).toStringList();
    const auto slot = combo->objectName().split(' ').last().toUInt();
    if (identity.size() != 2 || slot >= ids.size())
    {
        return false;
    }
    selection.protocol = identity[0].toStdString();
    ids[slot] = identity[1].toStdString();
    return true;
}
} // namespace

void MainWindow::changeLogValues(int tabIndex, const QString& protocolArg)
{
    QDialog dialog{this};
    auto *mainLayout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    mainLayout->addWidget(tabs);
    const auto addParameters =
        [&](const std::vector<std::string>& ids, const QString& title, const QString& kind, auto slot)
    {
        auto *page = new QWidget(tabs);
        auto *layout = new QGridLayout(page);
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            auto *combo = new QComboBox(page);
            combo->setObjectName(kind + " value " + QString::number(i));
            populateParameterChoices(*combo, *logger_model_, protocolArg, ids[i]);
            auto *units = new QComboBox(page);
            units->setObjectName(kind + " unit " + QString::number(i));
            if (const auto *p = logger_model_->Parameter(protocolArg.toStdString(), ids[i]); p != nullptr)
            {
                for (const auto& conversion : p->conversions)
                {
                    units->addItem(fastecu::ui::qs(conversion.units));
                }
            }
            auto *name = new QLineEdit(page);
            name->setObjectName(kind + " title " + QString::number(i));
            layout->addWidget(new QLabel(kind + " " + QString::number(i + 1), page), static_cast<int>(i), 0);
            layout->addWidget(combo, static_cast<int>(i), 1);
            layout->addWidget(units, static_cast<int>(i), 2);
            layout->addWidget(name, static_cast<int>(i), 3);
            connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, slot);
        }
        tabs->addTab(page, title);
    };
    const auto& selection = logger_model_->Selection();
    addParameters(selection.gauge_ids, "Gauges", "Gauge", &MainWindow::changeLogGaugeValue);
    addParameters(selection.lower_panel_ids, "Digital", "Digital", &MainWindow::changeLogDigitalValue);
    auto *switchPage = new QWidget(tabs);
    auto *switchLayout = new QGridLayout(switchPage);
    for (std::size_t i = 0; i < selection.switch_ids.size(); ++i)
    {
        auto *combo = new QComboBox(switchPage);
        combo->setObjectName("Switch value " + QString::number(i));
        populateSwitchChoices(*combo, *logger_model_, protocolArg, selection.switch_ids[i]);
        auto *name = new QLineEdit(switchPage);
        name->setObjectName("Switch title " + QString::number(i));
        switchLayout->addWidget(new QLabel("Switch " + QString::number(i + 1), switchPage), static_cast<int>(i), 0);
        switchLayout->addWidget(combo, static_cast<int>(i), 1);
        switchLayout->addWidget(name, static_cast<int>(i), 2);
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::changeLogSwitchValue);
    }
    tabs->addTab(switchPage, "Switches");
    tabs->setCurrentIndex(tabIndex);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    mainLayout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    // Edits are applied and saved immediately, preserving Cancel's behavior.
    dialog.exec();
}

void MainWindow::changeLogGaugeValue(int index)
{
    auto selection = logger_model_->Selection();
    if (applyChoice(selection, selection.gauge_ids, qobject_cast<QComboBox *>(sender()), index))
    {
        const auto protocol = QString::fromStdString(selection.protocol);
        logger_model_->SetSelection(std::move(selection));
        updateLogboxes(protocol);
        saveLoggerSelection();
    }
}
void MainWindow::changeLogDigitalValue(int index)
{
    auto selection = logger_model_->Selection();
    if (applyChoice(selection, selection.lower_panel_ids, qobject_cast<QComboBox *>(sender()), index))
    {
        const auto protocol = QString::fromStdString(selection.protocol);
        logger_model_->SetSelection(std::move(selection));
        updateLogboxes(protocol);
        saveLoggerSelection();
    }
}
void MainWindow::changeLogSwitchValue(int index)
{
    auto selection = logger_model_->Selection();
    if (applyChoice(selection, selection.switch_ids, qobject_cast<QComboBox *>(sender()), index))
    {
        const auto protocol = QString::fromStdString(selection.protocol);
        logger_model_->SetSelection(std::move(selection));
        updateLogboxes(protocol);
        saveLoggerSelection();
    }
}
