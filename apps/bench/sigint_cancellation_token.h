#pragma once

#include "src/backend/ports/cancellation.h"

namespace fastecu::bench
{

class SigintCancellationToken final : public ICancellationToken
{
  public:
    SigintCancellationToken();
    ~SigintCancellationToken() override;

    SigintCancellationToken(const SigintCancellationToken&) = delete;
    SigintCancellationToken& operator=(const SigintCancellationToken&) = delete;
    SigintCancellationToken(SigintCancellationToken&&) = delete;
    SigintCancellationToken& operator=(SigintCancellationToken&&) = delete;

    bool cancelled() const override;

  private:
    using SignalHandler = void (*)(int);
    SignalHandler previous_handler_;
};

} // namespace fastecu::bench
