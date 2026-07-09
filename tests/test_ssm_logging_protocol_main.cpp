#include "test_ssm_logging_protocol.h"

#include <QByteArray>

int main(int argc, char **argv)
{
    // This suite constructs a real QApplication (FileActions derives from
    // QWidget), which needs a display/desktop session to initialize its
    // platform plugin -- something headless CI runners don't have. The
    // offscreen platform plugin is Qt's own headless-safe substitute: no
    // window system dependency, no xvfb needed.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Temporary diagnostic: dump Qt's own platform-plugin resolution trace
    // to stderr, to see exactly what it tries/fails at on Windows CI, where
    // this suite hangs inside QApplication's constructor with no other output.
    qputenv("QT_DEBUG_PLUGINS", "1");
    return run_test_ssm_logging_protocol(argc, argv);
}
