#include <cstdio>

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
    // Run the QTest classes' output unbuffered. These suites exercise the
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

    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsSet("FASTECU_THROWING_BACKEND_CHILD"))
    {
        return run_throwing_backend_child();
    }

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
