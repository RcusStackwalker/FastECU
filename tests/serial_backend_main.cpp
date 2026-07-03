#include <QCoreApplication>
#include "test_direct_backend.h"
#include "test_remote_backend_smoke.h"
#include "test_facade_threading.h"
#ifdef Q_OS_UNIX
#include "test_pty_e2e.h"
#endif

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int status = 0;
    status |= run_test_direct_backend(argc, argv);
    status |= run_test_remote_backend_smoke(argc, argv);
    status |= run_test_facade_threading(argc, argv);
#ifdef Q_OS_UNIX
    status |= run_test_pty_e2e(argc, argv);
#endif
    return status;
}
