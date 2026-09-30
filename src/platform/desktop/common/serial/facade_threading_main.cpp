#include <cstdio>

#include "src/platform/desktop/common/testing/core_application_environment.h"

#include <gmock/gmock.h>

int run_throwing_backend_child();

int main(int argc, char **argv)
{
    // Run the suites' output unbuffered. These suites exercise the
    // serial facade's I/O-thread and QRemoteObjects teardown paths, which have
    // an intermittent, Windows-only crash (tracked separately). When Bazel
    // redirects stdout to test.log it is block-buffered, so a hard crash
    // (access violation, no CRT flush) discards the whole buffer and the
    // failing run shows *zero* output -- making the culprit slot impossible to
    // identify. Unbuffered stdout/stderr lands every "PASS : Class::slot()"
    // line as it happens, so the first slot with no trailing PASS/FAIL line is
    // exactly the one that crashed.
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
