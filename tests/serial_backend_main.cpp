#include <QCoreApplication>
#include "test_direct_backend.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int status = 0;
    status |= run_test_direct_backend(argc, argv);
    return status;
}
