#include "src/backend/logging/romraider_conversion.h"
#include <QRegularExpression>
#include <QStringList>

QString convertRomRaiderValue(FileActions *fileActions, FileActions::LogValuesStructure *logValues,
                              int j, const QString& rawValueString)
{
    QStringList conversion = logValues->log_value_units.at(j).split(",");
    const QString& from_byte = conversion.at(2);
    QStringList format_str_lst = conversion.at(3).split(".");
    uint8_t format = 0;
    if (format_str_lst.length() > 1)
    {
        format = format_str_lst.at(1).count(QRegularExpression("0"));
    }

    double calc_value = fileActions->calculate_value_from_expression(
        fileActions->parse_stringlist_from_expression_string(from_byte, rawValueString));
    return QString::number(calc_value, 'f', format);
}
