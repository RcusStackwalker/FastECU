#include "src/backend/flash/guarded_mixed_can_flash_transport.h"

namespace fastecu::flash
{

GuardedMixedCanFlashTransport::GuardedMixedCanFlashTransport(IMixedCanFlashTransport& inner) noexcept : inner_(inner)
{
}

Status GuardedMixedCanFlashTransport::ResetConnection()
{
    return inner_.ResetConnection();
}

Status GuardedMixedCanFlashTransport::Configure(const MixedCanConfig& config)
{
    return inner_.Configure(config);
}

Status GuardedMixedCanFlashTransport::Open()
{
    return inner_.Open();
}

Status GuardedMixedCanFlashTransport::Close()
{
    return inner_.Close();
}

Status GuardedMixedCanFlashTransport::EnterRawBootloaderMode()
{
    return inner_.EnterRawBootloaderMode();
}

Status GuardedMixedCanFlashTransport::ClearReceiveBuffer()
{
    return inner_.ClearReceiveBuffer();
}

Status GuardedMixedCanFlashTransport::EnterIso15765KernelMode()
{
    return inner_.EnterIso15765KernelMode();
}

void GuardedMixedCanFlashTransport::RequestUnblock() noexcept
{
    inner_.RequestUnblock();
}

Status GuardedMixedCanFlashTransport::WriteIso15765(bytes::ByteView data, const ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "mixed CAN ISO-15765 write cancelled before delegation");
    }
    return inner_.WriteIso15765(data, cancellation);
}

Result<std::optional<bytes::Bytes>> GuardedMixedCanFlashTransport::ReadIso15765(std::chrono::milliseconds timeout,
                                                                                const ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "mixed CAN ISO-15765 read cancelled before delegation");
    }
    return inner_.ReadIso15765(timeout, cancellation);
}

Status GuardedMixedCanFlashTransport::WriteRaw(const cdbg::CanFrame& frame, const ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "mixed CAN raw write cancelled before delegation");
    }
    return inner_.WriteRaw(frame, cancellation);
}

Result<std::optional<cdbg::CanFrame>> GuardedMixedCanFlashTransport::ReadRaw(std::chrono::milliseconds timeout,
                                                                             const ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "mixed CAN raw read cancelled before delegation");
    }
    return inner_.ReadRaw(timeout, cancellation);
}

} // namespace fastecu::flash
