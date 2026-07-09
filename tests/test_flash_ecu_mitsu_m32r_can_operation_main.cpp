#include "test_flash_ecu_mitsu_m32r_can_operation.h"

#include <QByteArray>

int main(int argc, char **argv)
{
    // This suite constructs a real QApplication (FileActions derives from
    // QWidget), which needs a display/desktop session to initialize its
    // platform plugin -- something headless CI runners don't have. The
    // offscreen platform plugin is Qt's own headless-safe substitute: no
    // window system dependency, no xvfb needed.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    return run_test_flash_ecu_mitsu_m32r_can_operation(argc, argv);
}
