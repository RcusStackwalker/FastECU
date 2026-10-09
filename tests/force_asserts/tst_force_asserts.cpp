// Proves QT_FORCE_ASSERTS re-arms Qt's bounds-check assertions under release
// (QT_NO_DEBUG) flags. The death-test statement does an out-of-range
// QByteArray access:
//   - asserts armed   -> Q_ASSERT -> qt_assert -> abort()  (SIGABRT, signal 6)
//   - asserts stripped -> Q_ASSERT no-op; verify() is a no-op; g_ba.d.ptr is
//                         nullptr (zero-initialized static), so data[0] is a
//                         null deref -> SIGSEGV (signal 11). RED state.
//
// g_ba is a file-scope static QByteArray so the optimizer cannot constant-fold
// its size into the at() call at compile time; access_out_of_bounds is noinline
// to prevent the statement from being optimized away via cross-function UB
// propagation (which happened with a local QByteArray).
#include <gtest/gtest.h>
#include <QByteArray>
#include <csignal>

// File-scope static: zero-initialized (ptr=nullptr, size=0). The optimizer
// cannot constant-fold this into access_out_of_bounds's at() call, so the
// function is emitted and the null-deref (SIGSEGV) or assert (SIGABRT) occurs
// at runtime.
static QByteArray g_ba;

__attribute__((noinline)) static void AccessOutOfBounds()
{
    // g_ba.size() == 0 at runtime; at(0) is out of bounds.
    // With QT_FORCE_ASSERTS: Q_ASSERT fires -> qt_assert -> abort() -> SIGABRT.
    // Without:               Q_ASSERT no-op; data[0] deref's nullptr -> SIGSEGV.
    volatile char c = g_ba.at(0);
    (void)c;
}

// The suite name ends in DeathTest so GoogleTest runs it before any test that
// has started threads.
TEST(ForceAssertsDeathTest, outOfBoundsAtAborts)
{
    // The process already has helper threads (GoogleTest reports three), so
    // re-execute the binary for the child instead of forking a threaded process.
    GTEST_FLAG_SET(death_test_style, "threadsafe");

    // The empty regex matches any message: the contract is the signal, and
    // KilledBySignal(SIGABRT) fails on the SIGSEGV of the stripped-asserts state.
    EXPECT_EXIT(AccessOutOfBounds(), ::testing::KilledBySignal(SIGABRT), "");
}
