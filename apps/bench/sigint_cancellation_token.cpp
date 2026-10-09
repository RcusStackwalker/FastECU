#include "apps/bench/sigint_cancellation_token.h"

#include <csignal>

namespace
{

volatile std::sig_atomic_t g_sigint_requested = 0;

extern "C" void HandleSigint(int /*signal*/)
{
    g_sigint_requested = 1;
}

} // namespace

namespace fastecu::bench
{

SigintCancellationToken::SigintCancellationToken()
{
    g_sigint_requested = 0;
    previous_handler_ = std::signal(SIGINT, HandleSigint);
}

SigintCancellationToken::~SigintCancellationToken()
{
    if (previous_handler_ != SIG_ERR)
    {
        std::signal(SIGINT, previous_handler_);
    }
}

bool SigintCancellationToken::Cancelled() const
{
    return g_sigint_requested != 0;
}

} // namespace fastecu::bench
