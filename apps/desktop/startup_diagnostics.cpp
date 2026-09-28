#include "apps/desktop/startup_diagnostics.h"

#include <QDebug>
#include <QMessageBox>

QString startup_failure_text(const fastecu::Error& error)
{
    return QStringLiteral("FastECU could not load its configuration and will exit.\n\n%1")
        .arg(QString::fromStdString(error.detail));
}

QString startup_warning_text(const QStringList& warnings)
{
    return QStringLiteral("FastECU started with configuration warnings:\n\n%1").arg(warnings.join("\n"));
}

void present_startup_failure(const fastecu::Error& error)
{
    qCritical().noquote() << startup_failure_text(error);
    QMessageBox::critical(nullptr, QStringLiteral("FastECU"), startup_failure_text(error));
}

void present_startup_warnings(const QStringList& warnings)
{
    if (warnings.isEmpty())
    {
        return;
    }
    qWarning().noquote() << startup_warning_text(warnings);
    QMessageBox::warning(nullptr, QStringLiteral("FastECU"), startup_warning_text(warnings));
}
