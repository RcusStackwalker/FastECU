#include "apps/desktop/startup_test_platform.h"

QString StartupMessageBoxTitle()
{
    // QMessageBox follows macOS policy: message boxes have no window title.
    return {};
}
