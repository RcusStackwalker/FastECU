#include "apps/desktop/startup_test_platform.h"

QString startup_message_box_title()
{
    // QMessageBox follows macOS policy: message boxes have no window title.
    return {};
}
