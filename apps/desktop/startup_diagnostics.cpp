#include "apps/desktop/startup_diagnostics.h"

#include <QDebug>
#include <QMessageBox>

QString StartupFailureText(const fastecu::Error& error)
{
    return QStringLiteral("FastECU could not load its configuration and will exit.\n\n%1")
        .arg(QString::fromStdString(error.detail));
}

QString StartupWarningText(const QStringList& warnings)
{
    return QStringLiteral("FastECU started with configuration warnings:\n\n%1").arg(warnings.join("\n"));
}

void PresentStartupFailure(const fastecu::Error& error)
{
    qCritical().noquote() << StartupFailureText(error);
    QMessageBox::critical(nullptr, QStringLiteral("FastECU"), StartupFailureText(error));
}

void PresentStartupWarnings(const QStringList& warnings)
{
    if (warnings.isEmpty())
    {
        return;
    }
    qWarning().noquote() << StartupWarningText(warnings);
    QMessageBox::warning(nullptr, QStringLiteral("FastECU"), StartupWarningText(warnings));
}
