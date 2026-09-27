#pragma once
#include <QString>
#include <QStringList>

#include "src/backend/ports/error.h"

// Startup has no window yet, so configuration failures and warnings are shown
// in standalone message boxes. The *_text functions are the testable part.
QString startup_failure_text(const fastecu::Error& error);
QString startup_warning_text(const QStringList& warnings);
void present_startup_failure(const fastecu::Error& error);
void present_startup_warnings(const QStringList& warnings); // no-op when empty
