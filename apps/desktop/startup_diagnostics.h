#pragma once
#include <QString>
#include <QStringList>

#include "src/backend/ports/error.h"

// Startup has no window yet, so configuration failures and warnings are shown
// in standalone message boxes. The *_text functions are the testable part.
QString StartupFailureText(const fastecu::Error& error);
QString StartupWarningText(const QStringList& warnings);
void PresentStartupFailure(const fastecu::Error& error);
void PresentStartupWarnings(const QStringList& warnings); // no-op when empty
