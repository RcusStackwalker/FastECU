#include <QCoreApplication>
#include "test_direct_backend.h"
#include "test_remote_backend_smoke.h"
#include "test_facade_threading.h"
#if defined(__unix__) || defined(__APPLE__)
#include "test_direct_backend_pty.h"
#include "test_pty_e2e.h"
#endif

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int status = 0;
    status |= run_test_direct_backend(argc, argv);
    status |= run_test_remote_backend_smoke(argc, argv);
    status |= run_test_facade_threading(argc, argv);
#if defined(__unix__) || defined(__APPLE__)
    status |= run_test_direct_backend_pty(argc, argv);
    status |= run_test_pty_e2e(argc, argv);
#endif
    return status;
}
