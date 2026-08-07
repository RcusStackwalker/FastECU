#pragma once

#include <QString>
#include <QStringList>

#include "src/backend/logging/logger_definition_model.h"

// Extracted from FileActions (it used to be a nested struct there) so that
// //src/backend/logging:legacy_logger_adapter can reference this value type
// without pulling in the whole FileActions QWidget. That split matters
// because it breaks a Bazel dependency cycle: legacy_logger_adapter needs
// this struct, and Task 7 makes //src/backend/definitions depend back on
// //src/backend/logging:legacy_logger_adapter -- if legacy_logger_adapter
// depended on the whole `definitions` target, that's a cycle Bazel rejects
// outright. FileActions re-exposes this type as `FileActions::LogValuesStructure`
// via a `using` alias (see file_actions.h) so no external call site's
// spelling changes. This mirrors the existing config_values.h / ecu_cal_def.h
// splits in this same directory.
namespace fastecu::definitions
{

struct LogValuesStructure
{
    QString ecu_id;
    QStringList log_value_protocol;
    QStringList log_value_id;
    QStringList log_value_name;
    QStringList log_value_description;
    QStringList log_value_ecu_byte_index;
    QStringList log_value_ecu_bit;
    QStringList log_value_target;
    QStringList log_value_address;
    QList<QList<fastecu::logging::Conversion>> log_value_conversions;
    QStringList log_value_from_byte;
    QStringList log_value_format;
    QStringList log_value_gauge_min;
    QStringList log_value_gauge_max;
    QStringList log_value_gauge_step;

    QStringList log_value_ecu_id;
    QStringList log_value_length;
    QStringList log_value_type;
    QStringList log_value;

    QStringList log_value_enabled;

    QStringList log_values_names_sorted;
    QStringList log_values_by_protocol;

    QStringList dashboard_log_value_id;
    QStringList lower_panel_log_value_id;
    QString logging_values_protocol;

    // Switch values
    QStringList log_switch_protocol;
    QStringList log_switch_id;
    QStringList log_switch_name;
    QStringList log_switch_description;
    QStringList log_switch_address;
    QStringList log_switch_ecu_byte_index;
    QStringList log_switch_ecu_bit;
    QStringList log_switch_target;
    QStringList log_switch_enabled;
    QStringList log_switch_state;

    QStringList log_switches_names_sorted;

    QStringList lower_panel_switch_id;
};

} // namespace fastecu::definitions
