#pragma once

#include <string>
#include <utility>

#include "src/backend/ports/result.h"

namespace fastecu
{

// Cooperative cancellation. Backend loops poll this between bounded operations;
// platform teardown flips it so an in-flight session unwinds promptly.
class ICancellationToken
{
  public:
    virtual ~ICancellationToken() = default;
    virtual bool Cancelled() const = 0;
};

// One poll as a Status step for and_then chains: kCancelled carrying `detail`.
inline Status CheckCancelled(const ICancellationToken& cancellation, std::string detail)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::move(detail));
    }
    return {};
}

} // namespace fastecu
