#include "src/backend/flash/guarded_can_flash_transport.h"

namespace fastecu::flash
{

GuardedCanFlashTransport::GuardedCanFlashTransport(ICanFlashTransport& inner, const ICancellationToken& cancellation)
    : inner_(inner), cancellation_(cancellation)
{
}

bool GuardedCanFlashTransport::Refuses() const
{
    return cancellation_.Cancelled();
}

Status GuardedCanFlashTransport::ResetConnection()
{
    return inner_.ResetConnection();
}

Status GuardedCanFlashTransport::Configure(const Iso15765Config& config)
{
    return inner_.Configure(config);
}

Status GuardedCanFlashTransport::Open()
{
    return inner_.Open();
}

Status GuardedCanFlashTransport::Close()
{
    return inner_.Close();
}

void GuardedCanFlashTransport::RequestUnblock() noexcept
{
    inner_.RequestUnblock();
}

Status GuardedCanFlashTransport::Write(bytes::ByteView data, const ICancellationToken& cancellation)
{
    if (Refuses())
    {
        return Fail(ErrorKind::kCancelled, "cancelled");
    }
    return inner_.Write(data, cancellation);
}

Result<std::optional<bytes::Bytes>> GuardedCanFlashTransport::Read(std::chrono::milliseconds timeout,
                                                                   const ICancellationToken& cancellation)
{
    if (Refuses())
    {
        return Fail(ErrorKind::kCancelled, "cancelled");
    }
    return inner_.Read(timeout, cancellation);
}

} // namespace fastecu::flash
