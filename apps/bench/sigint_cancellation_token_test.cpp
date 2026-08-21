#include "apps/bench/sigint_cancellation_token.h"

#include <gtest/gtest.h>

#include <csignal>

namespace fastecu::bench
{
namespace
{

using SignalHandler = void (*)(int);

class ScopedSigintHandler
{
  public:
    explicit ScopedSigintHandler(SignalHandler handler) : previous_(std::signal(SIGINT, handler))
    {
    }

    ~ScopedSigintHandler()
    {
        if (previous_ != SIG_ERR)
        {
            std::signal(SIGINT, previous_);
        }
    }

    ScopedSigintHandler(const ScopedSigintHandler&) = delete;
    ScopedSigintHandler& operator=(const ScopedSigintHandler&) = delete;

  private:
    SignalHandler previous_;
};

volatile std::sig_atomic_t g_previous_handler_calls = 0;

extern "C" void record_previous_handler(int /*signal*/)
{
    g_previous_handler_calls = 1;
}

TEST(SigintCancellationToken, SigintRequestsCancellation)
{
    const ScopedSigintHandler ignore_sigint(SIG_IGN);
    SigintCancellationToken cancellation;

    EXPECT_FALSE(cancellation.cancelled());
    ASSERT_EQ(std::raise(SIGINT), 0);
    EXPECT_TRUE(cancellation.cancelled());
}

TEST(SigintCancellationToken, DestructionRestoresThePreviousHandler)
{
    g_previous_handler_calls = 0;
    const ScopedSigintHandler record_sigint(record_previous_handler);
    {
        SigintCancellationToken cancellation;
        EXPECT_FALSE(cancellation.cancelled());
    }

    ASSERT_EQ(std::raise(SIGINT), 0);
    EXPECT_EQ(g_previous_handler_calls, 1);
}

} // namespace
} // namespace fastecu::bench
