#include "src/backend/diagnostics/ssm_identify.h"

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
    unsigned sum = 0;
    for (const bytes::Byte byte : data)
    {
        sum += byte;
    }
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
    return Fail(ErrorKind::kBadResponse, "SSM init response " + what + ": " + SpacedHex(frame));
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

Result<SsmIdentity> IdentifyKlineSsm2(IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
                                      SsmTarget target)
{
    // The state ssm_kline_init inherited from connect_to_ecu and
    // log_transport_changed, now set explicitly.
    if (auto opened = link.Open(KlineLinkConfig{.header = KlineHeader::kNone, .baud = 4800, .parity = Parity::kNone});
        !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    if (auto written = CheckWritten(link, SsmFrame(std::array<bytes::Byte, 1>{kSsmInitCommand}, target));
        !written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (auto slept = clock.Sleep(200ms, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }

    bytes::Bytes frame;
    if (auto got = ReadInto(link, frame, 200ms, cancellation); !got.has_value())
    {
        return std::unexpected(got.error());
    }
    for (int attempt = 0; frame.size() < 4 && attempt < 10; ++attempt)
    {
        if (auto got = ReadInto(link, frame, 50ms, cancellation); !got.has_value())
        {
            return std::unexpected(got.error());
        }
    }
    if (frame.empty())
    {
        return Fail(ErrorKind::kTimeout, "no SSM init response");
    }
    if (frame.size() < 4)
    {
        return BadFrame("header incomplete", frame);
    }
    const std::size_t declared = frame[3] + kSsmFrameOverhead;
    for (int attempt = 0; frame.size() < declared && attempt < 10; ++attempt)
    {
        if (auto got = ReadInto(link, frame, 50ms, cancellation); !got.has_value())
        {
            return std::unexpected(got.error());
        }
    }
    if (frame.size() < declared)
    {
        return BadFrame("truncated", frame);
    }
    frame.resize(declared); // the legacy code accepted len >= declared

    if (auto valid = ValidateSsm2Init(frame, target); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    auto ecu_id = ParseSsmEcuId(frame);
    if (!ecu_id.has_value())
    {
        return BadFrame("is too short for an ECU ID", frame);
    }
    return SsmIdentity{std::move(*ecu_id), std::move(frame)};
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

Result<SsmIdentity> IdentifySsm1(IDiagnosticLink& link, const ICancellationToken& cancellation)
{
    if (auto opened = link.Open(KlineLinkConfig{.header = KlineHeader::kNone, .baud = 1953, .parity = Parity::kEven});
        !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    // Every write is echo-checked: IDiagnosticLink has no other kind. The
    // legacy ssm_init sent the second and third without the echo check.
    if (auto woken =
            WriteThenDiscard(link, std::array<bytes::Byte, 4>{0x78, 0x12, 0x34, 0x00}, 10, 500ms, cancellation);
        !woken.has_value())
    {
        return std::unexpected(woken.error());
    }
    if (auto woken = WriteThenDiscard(link, std::array<bytes::Byte, 4>{0x00, 0x46, 0x48, 0x49}, 2, 500ms, cancellation);
        !woken.has_value())
    {
        return std::unexpected(woken.error());
    }
    if (auto written = CheckWritten(link, std::array<bytes::Byte, 4>{0x12, 0x00, 0x00, 0x00}); !written.has_value())
    {
        return std::unexpected(written.error());
    }

    bytes::Bytes frame;
    if (auto got = ReadInto(link, frame, 500ms, cancellation); !got.has_value())
    {
        return std::unexpected(got.error());
    }
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
    SsmIdentity identity{std::move(*ecu_id), std::move(frame)};

    // ssm_init parsed at most one more frame, unchecked, then drained.
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

Result<SsmIdentity> IdentifyIso15765Uds(IDiagnosticLink& link, const ICancellationToken& cancellation, SsmTarget target)
{
    const std::uint32_t source = target == SsmTarget::kEcu ? 0x7E0 : 0x7E1;
    if (auto opened = link.Open(CanLinkConfig{
            .iso15765 = true, .bitrate = 500000, .extended_id = false, .source_id = source, .destination_id = 0x7E8});
        !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    const std::array<bytes::Byte, 7> request{
        0x00, 0x00, static_cast<bytes::Byte>((source >> 8U) & 0xFFU), static_cast<bytes::Byte>(source & 0xFFU), 0x22,
        0xF1, 0x82};
    if (auto written = CheckWritten(link, request); !written.has_value())
    {
        return std::unexpected(written.error());
    }

    bytes::Bytes frame;
    if (auto got = ReadInto(link, frame, 100ms, cancellation); !got.has_value())
    {
        return std::unexpected(got.error());
    }
    if (frame.empty())
    {
        return Fail(ErrorKind::kTimeout, "no answer to ReadDataByIdentifier F182");
    }
    if (frame.size() <= 7 || frame[4] != 0x62 || frame[5] != 0xF1 || frame[6] != 0x82)
    {
        return Fail(ErrorKind::kBadResponse, "unexpected answer to ReadDataByIdentifier F182: " + SpacedHex(frame));
    }
    return SsmIdentity{UpperHex(bytes::ByteView(frame).subspan(7)), {}};
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
