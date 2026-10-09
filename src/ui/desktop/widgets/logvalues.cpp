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
void populate_choices(QComboBox& combo, std::vector<LoggerChoice> choices, const QString& protocol,
                      const std::string& selected)
{
    std::stable_sort(choices.begin(), choices.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    for (const auto& choice : choices)
    {
        combo.addItem(choice.name, choice.identity);
    }
    combo.setCurrentIndex(combo.findData(QStringList{protocol, QString::fromStdString(selected)}));
}
void populate_parameter_choices(QComboBox& combo, const fastecu::logging::LoggerModel& model, const QString& protocol,
                                const std::string& selected)
{
    std::vector<LoggerChoice> choices;
    const auto key = protocol.toStdString();
    for (const auto& p : model.definition().parameters)
    {
        if (p.protocol == key && model.parameter_supported(key, p.id))
        {
            choices.push_back({QString::fromStdString(p.name), {protocol, QString::fromStdString(p.id)}});
        }
    }
    populate_choices(combo, std::move(choices), protocol, selected);
}
void populate_switch_choices(QComboBox& combo, const fastecu::logging::LoggerModel& model, const QString& protocol,
                             const std::string& selected)
{
    std::vector<LoggerChoice> choices;
    const auto key = protocol.toStdString();
    for (const auto& p : model.definition().switches)
    {
        if (p.protocol == key && model.switch_supported(key, p.id))
        {
            choices.push_back({QString::fromStdString(p.name), {protocol, QString::fromStdString(p.id)}});
        }
    }
    populate_choices(combo, std::move(choices), protocol, selected);
}
bool apply_choice(fastecu::logging::LoggerSelection& selection, std::vector<std::string>& ids, const QComboBox *combo,
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

void MainWindow::change_log_values(int tab_index, const QString& protocol_arg)
{
    QDialog dialog{this};
    auto *main_layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    main_layout->addWidget(tabs);
    const auto add_parameters =
        [&](const std::vector<std::string>& ids, const QString& title, const QString& kind, auto slot)
    {
        auto *page = new QWidget(tabs);
        auto *layout = new QGridLayout(page);
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            auto *combo = new QComboBox(page);
            combo->setObjectName(kind + " value " + QString::number(i));
            populate_parameter_choices(*combo, *logger_model_, protocol_arg, ids[i]);
            auto *units = new QComboBox(page);
            units->setObjectName(kind + " unit " + QString::number(i));
            if (const auto *p = logger_model_->parameter(protocol_arg.toStdString(), ids[i]); p != nullptr)
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
    const auto& selection = logger_model_->selection();
    add_parameters(selection.gauge_ids, "Gauges", "Gauge", &MainWindow::change_log_gauge_value);
    add_parameters(selection.lower_panel_ids, "Digital", "Digital", &MainWindow::change_log_digital_value);
    auto *switch_page = new QWidget(tabs);
    auto *switch_layout = new QGridLayout(switch_page);
    for (std::size_t i = 0; i < selection.switch_ids.size(); ++i)
    {
        auto *combo = new QComboBox(switch_page);
        combo->setObjectName("Switch value " + QString::number(i));
        populate_switch_choices(*combo, *logger_model_, protocol_arg, selection.switch_ids[i]);
        auto *name = new QLineEdit(switch_page);
        name->setObjectName("Switch title " + QString::number(i));
        switch_layout->addWidget(new QLabel("Switch " + QString::number(i + 1), switch_page), static_cast<int>(i), 0);
        switch_layout->addWidget(combo, static_cast<int>(i), 1);
        switch_layout->addWidget(name, static_cast<int>(i), 2);
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::change_log_switch_value);
    }
    tabs->addTab(switch_page, "Switches");
    tabs->setCurrentIndex(tab_index);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    main_layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    // Edits are applied and saved immediately, preserving Cancel's behavior.
    dialog.exec();
}

void MainWindow::change_log_gauge_value(int index)
{
    auto selection = logger_model_->selection();
    if (apply_choice(selection, selection.gauge_ids, qobject_cast<QComboBox *>(sender()), index))
    {
        const auto protocol = QString::fromStdString(selection.protocol);
        logger_model_->set_selection(std::move(selection));
        update_logboxes(protocol);
        save_logger_selection();
    }
}
void MainWindow::change_log_digital_value(int index)
{
    auto selection = logger_model_->selection();
    if (apply_choice(selection, selection.lower_panel_ids, qobject_cast<QComboBox *>(sender()), index))
    {
        const auto protocol = QString::fromStdString(selection.protocol);
        logger_model_->set_selection(std::move(selection));
        update_logboxes(protocol);
        save_logger_selection();
    }
}
void MainWindow::change_log_switch_value(int index)
{
    auto selection = logger_model_->selection();
    if (apply_choice(selection, selection.switch_ids, qobject_cast<QComboBox *>(sender()), index))
    {
        const auto protocol = QString::fromStdString(selection.protocol);
        logger_model_->set_selection(std::move(selection));
        update_logboxes(protocol);
        save_logger_selection();
    }
}
