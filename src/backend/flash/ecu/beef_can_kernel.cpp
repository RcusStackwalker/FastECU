#include "src/backend/flash/ecu/beef_can_kernel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/backend/flash/ecu/denso_iso15765_crypto.h"

namespace fastecu::flash
{
namespace
{

constexpr std::size_t kUploadBlockSize = 128;

Result<bytes::Bytes> PreparePayload(bytes::ByteView kernel, std::string_view family)
{
    const std::size_t padded_to_word = (kernel.size() + 3U) & ~std::size_t{3U};
    const std::size_t block_count = (padded_to_word + kUploadBlockSize - 1U) / kUploadBlockSize;
    if (block_count == 0 || block_count > std::numeric_limits<std::size_t>::max() / kUploadBlockSize)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} kernel upload size is invalid", family));
    }
    bytes::Bytes plain(kernel.begin(), kernel.end());
    plain.resize(block_count * kUploadBlockSize - 4U, bytes::Byte{0});
    std::uint32_t sum = 0;
    for (std::size_t offset = 0; offset < plain.size(); offset += 4)
    {
        sum += bytes::ReadU32Be(plain, offset);
    }
    bytes::AppendU32Be(plain, 0x5AA5A55AU - sum);
    bytes::Bytes encrypted = DensoEncryptRom(plain);
    if (encrypted.size() >= 0x1000000U)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} kernel upload exceeds the 24-bit wire length", family));
    }
    return encrypted;
}

void LogDescribedFailure(IEventSink& events, const DensoBeefCanProfile& profile, const Error& failure)
{
    if (failure.kind == ErrorKind::kTimeout)
    {
        events.Log(LogLevel::kError, "No valid response from ECU");
    }
    else if (failure.kind == ErrorKind::kBadResponse)
    {
        events.Log(LogLevel::kError, std::format("Wrong response from {}: {}", profile.device, failure.detail));
    }
}

} // namespace

BeefCanKernel::BeefCanKernel(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                             IEventSink& events, uds::IUdsChannel& channel, const DensoBeefCanProfile& profile)
    : transport_(transport), cancellation_(cancellation), events_(events), channel_(channel), profile_(profile),
      uds_(channel, clock, events)
{
}

Status BeefCanKernel::TransferPayload(const KernelImage& kernel, const BeefKernelTransferPolicy& policy)
{
    const Result<bytes::Bytes> encrypted = PreparePayload(kernel.bytes, profile_.family);
    if (!encrypted.has_value())
    {
        return std::unexpected(encrypted.error());
    }
    events_.Log(LogLevel::kDebug, std::format("Start address to upload kernel: 0x{:x}", kernel.load_address));
    events_.Log(LogLevel::kInfo, "Initialize kernel upload");
    const auto request =
        bytes::ComposeBe(bytes::Byte{0x34}, bytes::Byte{0x04}, bytes::Byte{0x33}, bytes::U24{kernel.load_address},
                         bytes::U24{static_cast<std::uint32_t>(encrypted->size())});
    const Result<bytes::Bytes> reply = uds_.Request(request, policy.download_policy, cancellation_);
    if (!reply.has_value())
    {
        if (profile_.pdu_format == BeefPduFormat::kDescribe)
        {
            LogDescribedFailure(events_, profile_, reply.error());
        }
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (payload.empty() || payload.front() != 0x20)
    {
        const Error mismatch{ErrorKind::kBadResponse, "unexpected response during kernel download"};
        if (profile_.pdu_format == BeefPduFormat::kDescribe)
        {
            LogDescribedFailure(events_, profile_, mismatch);
        }
        else
        {
            events_.Log(LogLevel::kError,
                        std::format("Wrong response from {} during kernel download", profile_.device));
        }
        return std::unexpected(mismatch);
    }

    events_.Log(LogLevel::kInfo, "Uploading kernel, please wait...");
    // The <= boundary deliberately sends a final address-only B6 request.
    for (std::size_t offset = 0; offset <= encrypted->size(); offset += kUploadBlockSize)
    {
        if (cancellation_.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "kernel upload cancelled");
        }
        const bytes::ByteView chunk =
            bytes::ByteView{*encrypted}.subspan(offset, std::min(kUploadBlockSize, encrypted->size() - offset));
        if (const Status sent = channel_.Send(
                bytes::ComposeBe(bytes::Byte{0xB6},
                                 bytes::U24{kernel.load_address + static_cast<std::uint32_t>(offset)}, chunk),
                cancellation_);
            !sent.has_value())
        {
            return sent;
        }
        // Raw reads tolerate malformed, wrong-ID and stale adapter content.
        const Result<std::optional<bytes::Bytes>> ignored = transport_.Read(policy.block_reply_timeout, cancellation_);
        if (!ignored.has_value() &&
            (ignored.error().kind == ErrorKind::kCancelled || ignored.error().kind == ErrorKind::kDisconnected))
        {
            return std::unexpected(ignored.error());
        }
        if (cancellation_.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "kernel upload cancelled");
        }
    }
    return {};
}

} // namespace fastecu::flash
