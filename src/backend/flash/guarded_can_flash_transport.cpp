#include "src/backend/flash/guarded_can_flash_transport.h"

namespace fastecu::flash
{

GuardedCanFlashTransport::GuardedCanFlashTransport(ICanFlashTransport& inner) noexcept : inner_(inner)
{
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

Status GuardedCanFlashTransport::RestartIso15765(const Iso15765Config& config, const ICancellationToken& cancellation)
{
    return inner_.RestartIso15765(config, cancellation);
}

void GuardedCanFlashTransport::RequestUnblock() noexcept
{
    inner_.RequestUnblock();
}

Status GuardedCanFlashTransport::Write(bytes::ByteView data, const ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "CAN write cancelled before delegation");
    }
    return inner_.Write(data, cancellation);
}

Result<std::optional<bytes::Bytes>> GuardedCanFlashTransport::Read(std::chrono::milliseconds timeout,
                                                                   const ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "CAN read cancelled before delegation");
    }
    return inner_.Read(timeout, cancellation);
}

} // namespace fastecu::flash
