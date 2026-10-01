#include <cstdio>

#include "src/platform/desktop/common/testing/core_application_environment.h"

#include <gmock/gmock.h>

int run_throwing_backend_child();

int main(int argc, char **argv)
{
    // Preserve each GoogleTest diagnostic immediately when Bazel redirects
    // stdout/stderr to test.log, including during I/O-thread teardown.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    ::testing::InitGoogleMock(&argc, argv);
    if (qEnvironmentVariableIsSet("FASTECU_THROWING_BACKEND_CHILD"))
    {
        QCoreApplication app(argc, argv);
        return run_throwing_backend_child();
    }

    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
    return RUN_ALL_TESTS();
}
