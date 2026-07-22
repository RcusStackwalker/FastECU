#include "src/platform/desktop/common/ports/qt_settings.h"
#include <QSettings>
#include <QString>
#include <QVariant>

QtSettings::QtSettings() : settings_(std::make_unique<QSettings>())
{
}
QtSettings::~QtSettings() = default;

std::optional<std::string> QtSettings::get(std::string_view key) const
{
    QString k = QString::fromUtf8(key.data(), static_cast<int>(key.size()));
    if (!settings_->contains(k))
        return std::nullopt;
    return settings_->value(k).toString().toStdString();
}

void QtSettings::set(std::string_view key, std::string_view value)
{
    settings_->setValue(QString::fromUtf8(key.data(), static_cast<int>(key.size())),
                        QString::fromUtf8(value.data(), static_cast<int>(value.size())));
}
