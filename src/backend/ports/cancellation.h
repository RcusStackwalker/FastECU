#pragma once

namespace fastecu
{

// Cooperative cancellation. Backend loops poll this between bounded operations;
// platform teardown flips it so an in-flight session unwinds promptly.
class ICancellationToken
{
  public:
    virtual ~ICancellationToken() = default;
    virtual bool cancelled() const = 0;
};

} // namespace fastecu
