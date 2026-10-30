#include "src/backend/diagnostics/ssm_identify.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace fastecu::diagnostics
{
namespace
{

constexpr bytes::Byte kSsmHeader = 0x80;
constexpr bytes::Byte kSsmTester = 0xF0;
constexpr std::size_t kEcuIdOffset = 8;
constexpr std::size_t kEcuIdLength = 5;

bytes::Byte TargetId(SsmTarget target)
{
    return target == SsmTarget::kEcu ? 0x10 : 0x18;
}

bytes::Byte SsmChecksum(bytes::ByteView data)
{
    const unsigned sum =
        std::ranges::fold_left(data, 0U, [](unsigned total, const bytes::Byte byte) { return total + byte; });
    return static_cast<bytes::Byte>(sum & 0xFFU);
}

std::string UpperHex(bytes::ByteView data)
{
    std::string out;
    for (const bytes::Byte byte : data)
    {
        out += std::format("{:02X}", byte);
    }
    return out;
}

using namespace std::chrono_literals;

constexpr bytes::Byte kSsmInitCommand = 0xBF;
constexpr bytes::Byte kSsmInitResponse = 0xFF;
constexpr std::size_t kSsmFrameOverhead = 5; // header, tester, target, length, checksum

std::string SpacedHex(bytes::ByteView data)
{
    std::string out;
    for (const bytes::Byte byte : data)
    {
        out += std::format("{}{:02X}", out.empty() ? "" : " ", byte);
    }
    return out;
}

// Appends one read to `frame`. true when bytes arrived, false on a deadline.
Result<bool> ReadInto(IDiagnosticLink& link, bytes::Bytes& frame, std::chrono::milliseconds timeout,
                      const ICancellationToken& cancellation)
{
    auto read = link.Read(timeout, cancellation);
    if (!read.has_value())
    {
        return std::unexpected(read.error());
    }
    if (!read->has_value())
    {
        return false;
    }
    frame.insert(frame.end(), (*read)->begin(), (*read)->end());
    return true;
}

Status CheckWritten(IDiagnosticLink& link, bytes::ByteView request)
{
    auto written = link.Write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return {};
}

std::unexpected<Error> BadFrame(const std::string& what, bytes::ByteView frame)
{
    return Fail(ErrorKind::kBadResponse, std::format("SSM init response {}: {}", what, SpacedHex(frame)));
}

// RomRaider's SSMResponseProcessor.validateResponse, applied to a frame
// already cut to its declared length.
Status ValidateSsm2Init(bytes::ByteView frame, SsmTarget target)
{
    if (frame[3] == 0)
    {
        return BadFrame("has no response code", frame);
    }
    if (frame[0] != kSsmHeader)
    {
        return BadFrame(std::format("header byte is {:02X}, expected {:02X}", frame[0], kSsmHeader), frame);
    }
    if (frame[1] != kSsmTester)
    {
        return BadFrame(std::format("tester id is {:02X}, expected {:02X}", frame[1], kSsmTester), frame);
    }
    if (frame[2] != TargetId(target))
    {
        return BadFrame(std::format("target id is {:02X}, expected {:02X}", frame[2], TargetId(target)), frame);
    }
    if (frame[4] != kSsmInitResponse)
    {
        return BadFrame(std::format("response code is {:02X}, expected {:02X}", frame[4], kSsmInitResponse), frame);
    }
    const bytes::Byte expected = SsmChecksum(frame.first(frame.size() - 1));
    if (frame.back() != expected)
    {
        return BadFrame(std::format("checksum is {:02X}, expected {:02X}", frame.back(), expected), frame);
    }
    return {};
}

// Reads 50 ms at a time, at most ten times, until `frame` holds `size` bytes.
Status ReadUpTo(IDiagnosticLink& link, bytes::Bytes& frame, std::size_t size, const ICancellationToken& cancellation)
{
    for (int attempt = 0; frame.size() < size && attempt < 10; ++attempt)
    {
        if (auto got = ReadInto(link, frame, 50ms, cancellation); !got.has_value())
        {
            return std::unexpected(got.error());
        }
    }
    return {};
}

// The init reply, cut to its declared length: the legacy code accepted
// longer frames.
Result<bytes::Bytes> ReadSsm2InitFrame(IDiagnosticLink& link, const ICancellationToken& cancellation)
{
    bytes::Bytes frame;
    return ReadInto(link, frame, 200ms, cancellation)
        .and_then([&](bool) { return ReadUpTo(link, frame, 4, cancellation); })
        .and_then(
            [&]() -> Result<bytes::Bytes>
            {
                if (frame.empty())
                {
                    return Fail(ErrorKind::kTimeout, "no SSM init response");
                }
                if (frame.size() < 4)
                {
                    return BadFrame("header incomplete", frame);
                }
                const std::size_t declared = frame[3] + kSsmFrameOverhead;
                return ReadUpTo(link, frame, declared, cancellation)
                    .and_then(
                        [&]() -> Result<bytes::Bytes>
                        {
                            if (frame.size() < declared)
                            {
                                return BadFrame("truncated", frame);
                            }
                            frame.resize(declared);
                            return std::move(frame);
                        });
            });
}

Result<SsmIdentity> IdentifyKlineSsm2(IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
                                      SsmTarget target)
{
    // The state ssm_kline_init inherited from connect_to_ecu and
    // log_transport_changed, now set explicitly.
    return link.Open(KlineLinkConfig{.header = KlineHeader::kNone, .baud = 4800, .parity = Parity::kNone})
        .and_then([&] { return CheckWritten(link, SsmFrame(std::array<bytes::Byte, 1>{kSsmInitCommand}, target)); })
        .and_then([&] { return clock.Sleep(200ms, cancellation); })
        .and_then([&] { return ReadSsm2InitFrame(link, cancellation); })
        .and_then(
            [&](bytes::Bytes frame)
            {
                return ValidateSsm2Init(frame, target)
                    .and_then(
                        [&]() -> Result<SsmIdentity>
                        {
                            auto ecu_id = ParseSsmEcuId(frame);
                            if (!ecu_id.has_value())
                            {
                                return BadFrame("is too short for an ECU ID", frame);
                            }
                            return SsmIdentity{std::move(*ecu_id), std::move(frame)};
                        });
            });
}

constexpr int kSsm1MaxDrainReads = 100;

// Writes `request`, then reads `count` times for `timeout`, discarding what
// arrives: ssm_init logged these bytes and never used them.
Status WriteThenDiscard(IDiagnosticLink& link, bytes::ByteView request, int count, std::chrono::milliseconds timeout,
                        const ICancellationToken& cancellation)
{
    if (auto written = CheckWritten(link, request); !written.has_value())
    {
        return written;
    }
    bytes::Bytes discarded;
    for (int i = 0; i < count; ++i)
    {
        if (auto got = ReadInto(link, discarded, timeout, cancellation); !got.has_value())
        {
            return std::unexpected(got.error());
        }
    }
    return {};
}

// Every write is echo-checked: IDiagnosticLink has no other kind. The
// legacy ssm_init sent the second and third without the echo check.
Status WakeSsm1(IDiagnosticLink& link, const ICancellationToken& cancellation)
{
    return WriteThenDiscard(link, std::array<bytes::Byte, 4>{0x78, 0x12, 0x34, 0x00}, 10, 500ms, cancellation)
        .and_then(
            [&]
            {
                return WriteThenDiscard(link, std::array<bytes::Byte, 4>{0x00, 0x46, 0x48, 0x49}, 2, 500ms,
                                        cancellation);
            })
        .and_then([&] { return CheckWritten(link, std::array<bytes::Byte, 4>{0x12, 0x00, 0x00, 0x00}); });
}

// ssm_init parsed at most one more frame, unchecked, then drained.
Result<SsmIdentity> AdoptTrailingFrame(IDiagnosticLink& link, SsmIdentity identity,
                                       const ICancellationToken& cancellation)
{
    bytes::Bytes trailing;
    auto got = ReadInto(link, trailing, 100ms, cancellation);
    if (!got.has_value())
    {
        return std::unexpected(got.error());
    }
    if (!*got)
    {
        return identity;
    }
    if (auto trailing_id = ParseSsmEcuId(trailing); trailing_id.has_value())
    {
        identity = SsmIdentity{std::move(*trailing_id), std::move(trailing)};
    }
    for (int i = 0; i < kSsm1MaxDrainReads; ++i)
    {
        bytes::Bytes discarded;
        auto drained = ReadInto(link, discarded, 100ms, cancellation);
        if (!drained.has_value())
        {
            return std::unexpected(drained.error());
        }
        if (!*drained)
        {
            break;
        }
    }
    return identity;
}

Result<SsmIdentity> IdentifySsm1(IDiagnosticLink& link, const ICancellationToken& cancellation)
{
    bytes::Bytes frame;
    return link.Open(KlineLinkConfig{.header = KlineHeader::kNone, .baud = 1953, .parity = Parity::kEven})
        .and_then([&] { return WakeSsm1(link, cancellation); })
        .and_then([&] { return ReadInto(link, frame, 500ms, cancellation); })
        .and_then(
            [&](bool) -> Result<SsmIdentity>
            {
                if (frame.empty())
                {
                    return Fail(ErrorKind::kTimeout, "no SSM1 init response");
                }
                // Pinned: the length is the only check on SSM1.
                if (frame.size() < 4 || frame.size() != frame[3] + kSsmFrameOverhead)
                {
                    return BadFrame("length does not match its length byte", frame);
                }
                auto ecu_id = ParseSsmEcuId(frame);
                if (!ecu_id.has_value())
                {
                    return BadFrame("is too short for an ECU ID", frame);
                }
                return AdoptTrailingFrame(link, SsmIdentity{std::move(*ecu_id), std::move(frame)}, cancellation);
            });
}

Result<SsmIdentity> IdentifyIso15765Uds(IDiagnosticLink& link, const ICancellationToken& cancellation, SsmTarget target)
{
    const std::uint32_t source = target == SsmTarget::kEcu ? 0x7E0 : 0x7E1;
    const std::array<bytes::Byte, 7> request{
        0x00, 0x00, static_cast<bytes::Byte>((source >> 8U) & 0xFFU), static_cast<bytes::Byte>(source & 0xFFU), 0x22,
        0xF1, 0x82};
    bytes::Bytes frame;
    return link
        .Open(CanLinkConfig{
            .iso15765 = true, .bitrate = 500000, .extended_id = false, .source_id = source, .destination_id = 0x7E8})
        .and_then([&] { return CheckWritten(link, request); })
        .and_then([&] { return ReadInto(link, frame, 100ms, cancellation); })
        .and_then(
            [&](bool) -> Result<SsmIdentity>
            {
                if (frame.empty())
                {
                    return Fail(ErrorKind::kTimeout, "no answer to ReadDataByIdentifier F182");
                }
                if (frame.size() <= 7 || frame[4] != 0x62 || frame[5] != 0xF1 || frame[6] != 0x82)
                {
                    return Fail(ErrorKind::kBadResponse,
                                std::format("unexpected answer to ReadDataByIdentifier F182: {}", SpacedHex(frame)));
                }
                return SsmIdentity{UpperHex(bytes::ByteView(frame).subspan(7)), {}};
            });
}

} // namespace

bytes::Bytes SsmFrame(bytes::ByteView payload, SsmTarget target)
{
    bytes::Bytes frame{kSsmHeader, TargetId(target), kSsmTester, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(SsmChecksum(frame));
    return frame;
}

std::optional<std::string> ParseSsmEcuId(bytes::ByteView init_response)
{
    if (init_response.size() < kEcuIdOffset + kEcuIdLength)
    {
        return std::nullopt;
    }
    return UpperHex(init_response.subspan(kEcuIdOffset, kEcuIdLength));
}

Result<SsmIdentity> IdentifySsmEcu(IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
                                   const SsmIdentifyRequest& request)
{
    switch (request.variant)
    {
    case SsmVariant::kSsm1:
        return IdentifySsm1(link, cancellation);
    case SsmVariant::kKlineSsm2:
        return IdentifyKlineSsm2(link, clock, cancellation, request.target);
    case SsmVariant::kIso15765Uds:
        return IdentifyIso15765Uds(link, cancellation, request.target);
    }
    return Fail(ErrorKind::kInternal, "unknown SSM identification variant");
}

} // namespace fastecu::diagnostics
