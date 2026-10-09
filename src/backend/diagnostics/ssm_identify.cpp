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

bytes::Byte target_id(SsmTarget target)
{
    return target == SsmTarget::kEcu ? 0x10 : 0x18;
}

bytes::Byte ssm_checksum(bytes::ByteView data)
{
    unsigned sum = 0;
    for (const bytes::Byte byte : data)
    {
        sum += byte;
    }
    return static_cast<bytes::Byte>(sum & 0xFFU);
}

std::string upper_hex(bytes::ByteView data)
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

std::string spaced_hex(bytes::ByteView data)
{
    std::string out;
    for (const bytes::Byte byte : data)
    {
        out += std::format("{}{:02X}", out.empty() ? "" : " ", byte);
    }
    return out;
}

// Appends one read to `frame`. true when bytes arrived, false on a deadline.
Result<bool> read_into(IDiagnosticLink& link, bytes::Bytes& frame, std::chrono::milliseconds timeout,
                       const ICancellationToken& cancellation)
{
    auto read = link.read(timeout, cancellation);
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

Status check_written(IDiagnosticLink& link, bytes::ByteView request)
{
    auto written = link.write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return {};
}

std::unexpected<Error> bad_frame(const std::string& what, bytes::ByteView frame)
{
    return fail(ErrorKind::kBadResponse, "SSM init response " + what + ": " + spaced_hex(frame));
}

// RomRaider's SSMResponseProcessor.validateResponse, applied to a frame
// already cut to its declared length.
Status validate_ssm2_init(bytes::ByteView frame, SsmTarget target)
{
    if (frame[3] == 0)
    {
        return bad_frame("has no response code", frame);
    }
    if (frame[0] != kSsmHeader)
    {
        return bad_frame(std::format("header byte is {:02X}, expected {:02X}", frame[0], kSsmHeader), frame);
    }
    if (frame[1] != kSsmTester)
    {
        return bad_frame(std::format("tester id is {:02X}, expected {:02X}", frame[1], kSsmTester), frame);
    }
    if (frame[2] != target_id(target))
    {
        return bad_frame(std::format("target id is {:02X}, expected {:02X}", frame[2], target_id(target)), frame);
    }
    if (frame[4] != kSsmInitResponse)
    {
        return bad_frame(std::format("response code is {:02X}, expected {:02X}", frame[4], kSsmInitResponse), frame);
    }
    const bytes::Byte expected = ssm_checksum(frame.first(frame.size() - 1));
    if (frame.back() != expected)
    {
        return bad_frame(std::format("checksum is {:02X}, expected {:02X}", frame.back(), expected), frame);
    }
    return {};
}

Result<SsmIdentity> identify_kline_ssm2(IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
                                        SsmTarget target)
{
    // The state ssm_kline_init inherited from connect_to_ecu and
    // log_transport_changed, now set explicitly.
    if (auto opened = link.open(KlineLinkConfig{.header = KlineHeader::kNone, .baud = 4800, .parity = Parity::kNone});
        !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    if (auto written = check_written(link, ssm_frame(std::array<bytes::Byte, 1>{kSsmInitCommand}, target));
        !written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (auto slept = clock.sleep(200ms, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }

    bytes::Bytes frame;
    if (auto got = read_into(link, frame, 200ms, cancellation); !got.has_value())
    {
        return std::unexpected(got.error());
    }
    for (int attempt = 0; frame.size() < 4 && attempt < 10; ++attempt)
    {
        if (auto got = read_into(link, frame, 50ms, cancellation); !got.has_value())
        {
            return std::unexpected(got.error());
        }
    }
    if (frame.empty())
    {
        return fail(ErrorKind::kTimeout, "no SSM init response");
    }
    if (frame.size() < 4)
    {
        return bad_frame("header incomplete", frame);
    }
    const std::size_t declared = frame[3] + kSsmFrameOverhead;
    for (int attempt = 0; frame.size() < declared && attempt < 10; ++attempt)
    {
        if (auto got = read_into(link, frame, 50ms, cancellation); !got.has_value())
        {
            return std::unexpected(got.error());
        }
    }
    if (frame.size() < declared)
    {
        return bad_frame("truncated", frame);
    }
    frame.resize(declared); // the legacy code accepted len >= declared

    if (auto valid = validate_ssm2_init(frame, target); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    auto ecu_id = parse_ssm_ecu_id(frame);
    if (!ecu_id.has_value())
    {
        return bad_frame("is too short for an ECU ID", frame);
    }
    return SsmIdentity{std::move(*ecu_id), std::move(frame)};
}

constexpr int kSsm1MaxDrainReads = 100;

// Writes `request`, then reads `count` times for `timeout`, discarding what
// arrives: ssm_init logged these bytes and never used them.
Status write_then_discard(IDiagnosticLink& link, bytes::ByteView request, int count, std::chrono::milliseconds timeout,
                          const ICancellationToken& cancellation)
{
    if (auto written = check_written(link, request); !written.has_value())
    {
        return written;
    }
    bytes::Bytes discarded;
    for (int i = 0; i < count; ++i)
    {
        if (auto got = read_into(link, discarded, timeout, cancellation); !got.has_value())
        {
            return std::unexpected(got.error());
        }
    }
    return {};
}

Result<SsmIdentity> identify_ssm1(IDiagnosticLink& link, const ICancellationToken& cancellation)
{
    if (auto opened = link.open(KlineLinkConfig{.header = KlineHeader::kNone, .baud = 1953, .parity = Parity::kEven});
        !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    // Every write is echo-checked: IDiagnosticLink has no other kind. The
    // legacy ssm_init sent the second and third without the echo check.
    if (auto woken =
            write_then_discard(link, std::array<bytes::Byte, 4>{0x78, 0x12, 0x34, 0x00}, 10, 500ms, cancellation);
        !woken.has_value())
    {
        return std::unexpected(woken.error());
    }
    if (auto woken =
            write_then_discard(link, std::array<bytes::Byte, 4>{0x00, 0x46, 0x48, 0x49}, 2, 500ms, cancellation);
        !woken.has_value())
    {
        return std::unexpected(woken.error());
    }
    if (auto written = check_written(link, std::array<bytes::Byte, 4>{0x12, 0x00, 0x00, 0x00}); !written.has_value())
    {
        return std::unexpected(written.error());
    }

    bytes::Bytes frame;
    if (auto got = read_into(link, frame, 500ms, cancellation); !got.has_value())
    {
        return std::unexpected(got.error());
    }
    if (frame.empty())
    {
        return fail(ErrorKind::kTimeout, "no SSM1 init response");
    }
    // Pinned: the length is the only check on SSM1.
    if (frame.size() < 4 || frame.size() != frame[3] + kSsmFrameOverhead)
    {
        return bad_frame("length does not match its length byte", frame);
    }
    auto ecu_id = parse_ssm_ecu_id(frame);
    if (!ecu_id.has_value())
    {
        return bad_frame("is too short for an ECU ID", frame);
    }
    SsmIdentity identity{std::move(*ecu_id), std::move(frame)};

    // ssm_init parsed at most one more frame, unchecked, then drained.
    bytes::Bytes trailing;
    auto got = read_into(link, trailing, 100ms, cancellation);
    if (!got.has_value())
    {
        return std::unexpected(got.error());
    }
    if (!*got)
    {
        return identity;
    }
    if (auto trailing_id = parse_ssm_ecu_id(trailing); trailing_id.has_value())
    {
        identity = SsmIdentity{std::move(*trailing_id), std::move(trailing)};
    }
    for (int i = 0; i < kSsm1MaxDrainReads; ++i)
    {
        bytes::Bytes discarded;
        auto drained = read_into(link, discarded, 100ms, cancellation);
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

Result<SsmIdentity> identify_iso15765_uds(IDiagnosticLink& link, const ICancellationToken& cancellation,
                                          SsmTarget target)
{
    const std::uint32_t source = target == SsmTarget::kEcu ? 0x7E0 : 0x7E1;
    if (auto opened = link.open(CanLinkConfig{
            .iso15765 = true, .bitrate = 500000, .extended_id = false, .source_id = source, .destination_id = 0x7E8});
        !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    const std::array<bytes::Byte, 7> request{
        0x00, 0x00, static_cast<bytes::Byte>((source >> 8U) & 0xFFU), static_cast<bytes::Byte>(source & 0xFFU), 0x22,
        0xF1, 0x82};
    if (auto written = check_written(link, request); !written.has_value())
    {
        return std::unexpected(written.error());
    }

    bytes::Bytes frame;
    if (auto got = read_into(link, frame, 100ms, cancellation); !got.has_value())
    {
        return std::unexpected(got.error());
    }
    if (frame.empty())
    {
        return fail(ErrorKind::kTimeout, "no answer to ReadDataByIdentifier F182");
    }
    if (frame.size() <= 7 || frame[4] != 0x62 || frame[5] != 0xF1 || frame[6] != 0x82)
    {
        return fail(ErrorKind::kBadResponse, "unexpected answer to ReadDataByIdentifier F182: " + spaced_hex(frame));
    }
    return SsmIdentity{upper_hex(bytes::ByteView(frame).subspan(7)), {}};
}

} // namespace

bytes::Bytes ssm_frame(bytes::ByteView payload, SsmTarget target)
{
    bytes::Bytes frame{kSsmHeader, target_id(target), kSsmTester, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(ssm_checksum(frame));
    return frame;
}

std::optional<std::string> parse_ssm_ecu_id(bytes::ByteView init_response)
{
    if (init_response.size() < kEcuIdOffset + kEcuIdLength)
    {
        return std::nullopt;
    }
    return upper_hex(init_response.subspan(kEcuIdOffset, kEcuIdLength));
}

Result<SsmIdentity> identify_ssm_ecu(IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
                                     const SsmIdentifyRequest& request)
{
    switch (request.variant)
    {
    case SsmVariant::kSsm1:
        return identify_ssm1(link, cancellation);
    case SsmVariant::kKlineSsm2:
        return identify_kline_ssm2(link, clock, cancellation, request.target);
    case SsmVariant::kIso15765Uds:
        return identify_iso15765_uds(link, cancellation, request.target);
    }
    return fail(ErrorKind::kInternal, "unknown SSM identification variant");
}

} // namespace fastecu::diagnostics
