#include <QCoreApplication>
#include "test_direct_backend.h"
#include "test_remote_backend_smoke.h"
#include "test_facade_threading.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int status = 0;
    status |= run_test_direct_backend(argc, argv);
    status |= run_test_remote_backend_smoke(argc, argv);
    status |= run_test_facade_threading(argc, argv);
    return status;
}
