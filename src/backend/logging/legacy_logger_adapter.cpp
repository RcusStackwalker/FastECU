#include "src/backend/logging/legacy_logger_adapter.h"

#include <QString>

namespace fastecu::logging
{
namespace
{

QString qstr(const std::string& value)
{
    return QString::fromStdString(value);
}

QStringList to_qstringlist(const std::vector<std::string>& values)
{
    QStringList list;
    list.reserve(static_cast<qsizetype>(values.size()));
    for (const std::string& value : values)
    {
        list.append(qstr(value));
    }
    return list;
}

} // namespace

void apply_definition(
    const LoggerDefinition& definition, definitions::LogValuesStructure& values)
{
    for (const LoggerParameter& parameter : definition.parameters)
    {
        values.log_value_protocol.append(qstr(parameter.protocol));
        values.log_value_id.append(qstr(parameter.id));
        values.log_value_name.append(qstr(parameter.name));
        values.log_value_description.append(qstr(parameter.description));
        values.log_value_ecu_byte_index.append(qstr(parameter.ecu_byte_index));
        values.log_value_ecu_bit.append(qstr(parameter.ecu_bit));
        values.log_value_target.append(qstr(parameter.target));
        values.log_value_address.append(qstr(parameter.address));
        values.log_value_length.append(qstr(parameter.length));
        values.log_value_enabled.append(parameter.enabled ? "1" : "0");
        values.log_value.append("0.00");

        QString packed;
        for (std::size_t i = 0; i < parameter.conversions.size(); i++)
        {
            const Conversion& c = parameter.conversions.at(i);
            if (i > 0)
            {
                packed.append(",");
            }
            packed.append(QString("conversion %1,").arg(i));
            packed.append(qstr(c.units) + ",");
            packed.append(qstr(c.expr) + ",");
            packed.append(qstr(c.format) + ",");
            packed.append(qstr(c.gauge_min) + ",");
            packed.append(qstr(c.gauge_max) + ",");
            packed.append(qstr(c.gauge_step));
        }
        values.log_value_units.append(packed);
    }

    for (const LoggerSwitch& paramswitch : definition.switches)
    {
        values.log_switch_protocol.append(qstr(paramswitch.protocol));
        values.log_switch_id.append(qstr(paramswitch.id));
        values.log_switch_name.append(qstr(paramswitch.name));
        values.log_switch_description.append(qstr(paramswitch.description));
        values.log_switch_address.append(qstr(paramswitch.address));
        values.log_switch_ecu_byte_index.append(qstr(paramswitch.ecu_byte_index));
        values.log_switch_ecu_bit.append(qstr(paramswitch.ecu_bit));
        values.log_switch_target.append(qstr(paramswitch.target));
        values.log_switch_enabled.append(paramswitch.enabled ? "1" : "0");
        values.log_switch_state.append("0");
    }
}

void apply_selection(
    const LoggerSelection& selection, definitions::LogValuesStructure& values)
{
    values.logging_values_protocol = qstr(selection.protocol);
    values.dashboard_log_value_id = to_qstringlist(selection.gauge_ids);
    values.lower_panel_log_value_id = to_qstringlist(selection.lower_panel_ids);
    values.lower_panel_switch_id = to_qstringlist(selection.switch_ids);
}

} // namespace fastecu::logging
