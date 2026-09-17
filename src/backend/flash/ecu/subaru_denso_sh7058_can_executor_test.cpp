#include "src/backend/flash/ecu/subaru_denso_sh7058_can_executor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

using ::testing::Contains;
using ::testing::ElementsAre;

namespace fastecu
{

bool operator==(const RecordedPhaseProgress& left, const RecordedPhaseProgress& right)
{
    return left.phase_name == right.phase_name && left.phase_index == right.phase_index &&
           left.phase_count == right.phase_count && left.done == right.done && left.total == right.total;
}

} // namespace fastecu

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

constexpr std::uint32_t kRomSize = 0x00100000;
constexpr std::uint32_t kKernelAddress = 0xFFFF3000;
constexpr std::uint32_t kReadPageSize = 0x400;
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitSize = 0x1000;

enum class UploadB6Reply
{
    NoFrame,
    Timeout,
    Short,
    Malformed,
    WrongCanId,
    AdapterError,
    Cancelled,
    Disconnected,
};

struct Variant
{
    std::string_view protocol;
    SubaruDensoSh7058CanSecurity security;
    std::array<bytes::Byte, 4> expected_key;
};

// Fixed vectors for seed 11 22 33 44. No production table or seed helper is
// used to derive these expected bytes.
constexpr std::array<Variant, 5> kVariants{{
    {"sub_ecu_denso_sh7058_can", SubaruDensoSh7058CanSecurity::Stock, {0x35, 0xB6, 0x83, 0xBF}},
    {"sub_ecu_denso_sh7058_can_ecutek", SubaruDensoSh7058CanSecurity::EcuTek, {0xAD, 0xD9, 0x60, 0xEE}},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom", SubaruDensoSh7058CanSecurity::RaceRom, {0x05, 0x88, 0x2B, 0x0C}},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", SubaruDensoSh7058CanSecurity::RaceRomAlt, {0xFF, 0x90, 0x1D, 0xD4}},
    {"sub_ecu_denso_sh7058_can_cobb", SubaruDensoSh7058CanSecurity::Cobb, {0x2C, 0xD1, 0x8A, 0xEE}},
}};

struct BlockFixture
{
    std::uint32_t start;
    std::uint32_t length;
    std::uint32_t zero_crc;
};

// Literal SH7058 geometry and independently fixed zero-image CRCs. Neither
// the production flash-device table nor production CRC code builds these.
constexpr std::array<BlockFixture, 16> kBlocks{{
    {0x00000000, 0x00001000, 0xF722EF49},
    {0x00001000, 0x00001000, 0xF722EF49},
    {0x00002000, 0x00001000, 0xF722EF49},
    {0x00003000, 0x00001000, 0xF722EF49},
    {0x00004000, 0x00001000, 0xF722EF49},
    {0x00005000, 0x00001000, 0xF722EF49},
    {0x00006000, 0x00001000, 0xF722EF49},
    {0x00007000, 0x00001000, 0xF722EF49},
    {0x00008000, 0x00018000, 0xC85F0DED},
    {0x00020000, 0x00020000, 0xC39BE4A8},
    {0x00040000, 0x00020000, 0xC39BE4A8},
    {0x00060000, 0x00020000, 0xC39BE4A8},
    {0x00080000, 0x00020000, 0xC39BE4A8},
    {0x000A0000, 0x00020000, 0xC39BE4A8},
    {0x000C0000, 0x00020000, 0xC39BE4A8},
    {0x000E0000, 0x00020000, 0xC39BE4A8},
}};

class NeverCancelled final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return false;
    }
};

class ToggleCancellation final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return cancelled_.load();
    }
    void cancel()
    {
        cancelled_.store(true);
    }

  private:
    std::atomic_bool cancelled_{false};
};

class RecordingClock final : public FakeClock
{
  public:
    Status sleep(std::chrono::milliseconds duration, const ICancellationToken& cancellation) override
    {
        sleeps.push_back(duration);
        return FakeClock::sleep(duration, cancellation);
    }

    std::vector<std::chrono::milliseconds> sleeps;
};

class RecordingCanTransport final : public ICanFlashTransport
{
  public:
    Status reset_connection() override
    {
        lifecycle.push_back("reset_connection");
        Status result = reset_result;
        if (result.has_value() && cancellation_on_reset != nullptr)
        {
            cancellation_on_reset->cancel();
        }
        return result;
    }
    Status configure(const Iso15765Config& config) override
    {
        lifecycle.push_back("configure");
        Status result = scripted.configure(config);
        if (result.has_value() && cancellation_on_configure != nullptr)
        {
            cancellation_on_configure->cancel();
        }
        return result;
    }
    Status open() override
    {
        lifecycle.push_back("open");
        Status result = scripted.open();
        if (result.has_value() && cancellation_on_open != nullptr)
        {
            cancellation_on_open->cancel();
        }
        return result;
    }
    Status close() override
    {
        lifecycle.push_back("close");
        return scripted.close();
    }
    void request_unblock() noexcept override
    {
        scripted.request_unblock();
    }
    Status write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        writes.emplace_back(data.begin(), data.end());
        Status result = scripted.write(data, cancellation);
        if (result.has_value() && cancellation_to_trigger != nullptr && !cancel_prefix.empty() &&
            data.size() >= cancel_prefix.size() && std::equal(cancel_prefix.begin(), cancel_prefix.end(), data.begin()))
        {
            cancellation_to_trigger->cancel();
        }
        return result;
    }
    Result<std::optional<bytes::Bytes>> read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override
    {
        read_timeouts.push_back(timeout);
        return scripted.read(timeout, cancellation);
    }

    ScriptedCanFlashTransport scripted;
    Status reset_result;
    std::vector<std::string> lifecycle;
    std::vector<bytes::Bytes> writes;
    std::vector<std::chrono::milliseconds> read_timeouts;
    ToggleCancellation *cancellation_to_trigger = nullptr;
    ToggleCancellation *cancellation_on_reset = nullptr;
    ToggleCancellation *cancellation_on_configure = nullptr;
    ToggleCancellation *cancellation_on_open = nullptr;
    bytes::Bytes cancel_prefix;
};

class PhaseCancellingEventSink final : public RecordingEventSink
{
  public:
    PhaseCancellingEventSink(ToggleCancellation& cancellation, std::string phase, int done)
        : cancellation_(cancellation), phase_(std::move(phase)), done_(done)
    {
    }

    void phase_progress(const PhaseProgressEvent& event) override
    {
        RecordingEventSink::phase_progress(event);
        if (event.phase_name == phase_ && event.done == done_)
        {
            cancellation_.cancel();
        }
    }

  private:
    ToggleCancellation& cancellation_;
    std::string phase_;
    int done_;
};

class EraseCancellingEventSink final : public RecordingEventSink
{
  public:
    explicit EraseCancellingEventSink(ToggleCancellation& cancellation) : cancellation_(cancellation)
    {
    }

    void log(LogLevel level, std::string_view message) override
    {
        RecordingEventSink::log(level, message);
        if (message == " erased")
        {
            cancellation_.cancel();
        }
    }

  private:
    ToggleCancellation& cancellation_;
};

KernelImage kernel(bytes::Bytes data = {0x01, 0x02, 0x03, 0x04})
{
    return {.id = "petrol-sh7058-kernel", .load_address = kKernelAddress, .bytes = std::move(data)};
}

Result<FlashPlan> plan_for(const Variant& variant, FlashOperation operation, bytes::Bytes image = {},
                           bytes::Bytes kernel_data = {0x01, 0x02, 0x03, 0x04})
{
    std::optional<bytes::Bytes> selected_image;
    if (operation != FlashOperation::Read)
    {
        if (image.empty())
        {
            image.assign(kRomSize, bytes::Byte{0});
        }
        selected_image = std::move(image);
    }
    return build_subaru_denso_sh7058_can_plan(operation, variant.protocol, "SH7058", std::move(selected_image),
                                              kernel(std::move(kernel_data)));
}

bytes::Bytes request(bytes::ByteView pdu)
{
    bytes::Bytes wire{0x00, 0x00, 0x07, 0xE0};
    wire.insert(wire.end(), pdu.begin(), pdu.end());
    return wire;
}

bytes::Bytes response(bytes::ByteView pdu)
{
    bytes::Bytes wire{0x00, 0x00, 0x07, 0xE8};
    wire.insert(wire.end(), pdu.begin(), pdu.end());
    return wire;
}

bytes::Bytes be16(std::uint16_t value)
{
    const auto unsigned_value = static_cast<std::uint32_t>(value);
    return {static_cast<bytes::Byte>(unsigned_value >> 8U), static_cast<bytes::Byte>(unsigned_value)};
}

bytes::Bytes be32(std::uint32_t value)
{
    return {static_cast<bytes::Byte>(value >> 24U), static_cast<bytes::Byte>(value >> 16U),
            static_cast<bytes::Byte>(value >> 8U), static_cast<bytes::Byte>(value)};
}

bytes::Bytes beef_request(bytes::Byte opcode, bytes::ByteView payload = {})
{
    const auto payload_size = static_cast<std::uint64_t>(payload.size()) + 1U;
    bytes::Bytes pdu{0xBE, 0xEF, static_cast<bytes::Byte>(payload_size >> 8U),
                     static_cast<bytes::Byte>(payload.size() + 1), opcode};
    pdu.insert(pdu.end(), payload.begin(), payload.end());
    return request(pdu);
}

bytes::Bytes beef_response(bytes::Byte opcode, bytes::ByteView payload = {})
{
    const auto payload_size = static_cast<std::uint64_t>(payload.size()) + 1U;
    bytes::Bytes pdu{0xBE, 0xEF, static_cast<bytes::Byte>(payload_size >> 8U),
                     static_cast<bytes::Byte>(payload.size() + 1), opcode};
    pdu.insert(pdu.end(), payload.begin(), payload.end());
    return response(pdu);
}

bytes::Bytes kernel_id_request()
{
    return {0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
}

bytes::Bytes kernel_id_response()
{
    return {0x00, 0x00, 0x07, 0xE8, 0xBE, 0xEF, 0x00, 0x04, 0x41, 0x4B, 0x49, 0x44};
}

void configure_and_open(SubaruDensoSh7058CanExecutor& executor, const FlashPlan& plan, ICanFlashTransport& transport)
{
    auto setup = executor.transport_setup(plan);
    ASSERT_TRUE(setup.has_value()) << setup.error().detail;
    ASSERT_TRUE(transport.configure(*setup).has_value());
    ASSERT_TRUE(transport.open().has_value());
}

void script_kernel_alive(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(kernel_id_request());
    transport.queueRead(kernel_id_response());
}

void script_kernel_probe_timeout(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(kernel_id_request());
    transport.queue_no_frame();
}

void script_identity_queries(ScriptedCanFlashTransport& transport, std::string_view cal_id = "CALID")
{
    // All four successful payloads are literal legacy layouts. The leading
    // filler byte(s) are retained because the legacy extracts identity data
    // from fixed offsets after the ISO envelope.
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xAA});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xEA, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x9A});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x09, 0x02});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x49, 0x02, 0x00, 0x56, 0x49, 0x4E});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x09, 0x04});
    bytes::Bytes cal{0x00, 0x00, 0x07, 0xE8, 0x49, 0x04, 0x00};
    cal.insert(cal.end(), cal_id.begin(), cal_id.end());
    transport.queueRead(cal);
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x09, 0x06});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x49, 0x06, 0x00, 0x12, 0x34});
}

void script_tolerated_identity_failures(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xAA});
    transport.queue_no_frame();
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x09, 0x02});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x7F, 0x09, 0x12});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x09, 0x04});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x49});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x09, 0x06});
    transport.queue_error(ErrorKind::Timeout, "tolerated CVN timeout");
}

void script_racerom_alt_ram(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xA8, 0x00, 0xFF, 0x1E, 0xD8, 0xFF, 0x1E, 0xD9, 0xFF,
                                       0x1E, 0xDA, 0xFF, 0x1E, 0xDB});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xE8, 0x01, 0x02, 0x03, 0x04});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xA8, 0x00, 0xFF, 0x1E, 0x80, 0xFF, 0x1E, 0x81, 0xFF,
                                       0x1E, 0x82, 0xFF, 0x1E, 0x83});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xE8, 0x00, 0x00, 0x12, 0x34});
}

void script_session_selection(ScriptedCanFlashTransport& transport, bool use_42 = false)
{
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x10, 0x03});
    if (use_42)
    {
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x7F, 0x10, 0x12});
    }
    else
    {
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x50, 0x03});
    }
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x10, 0x43});
    if (use_42)
    {
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x50, 0x43});
    }
    else
    {
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x7F, 0x10, 0x12});
    }
}

void script_seed_and_programming(ScriptedCanFlashTransport& transport, const std::array<bytes::Byte, 4>& key,
                                 bool use_42 = false, bool accept_key = true)
{
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x01});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x67, 0x01, 0x11, 0x22, 0x33, 0x44});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x02, key[0], key[1], key[2], key[3]});
    if (!accept_key)
    {
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x7F, 0x27, 0x35});
        return;
    }
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x67, 0x02});
    if (use_42)
    {
        transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x10, 0x42});
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x50, 0x42});
    }
    else
    {
        transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x10, 0x02});
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x50, 0x02});
    }
}

void script_bootloader_connection(ScriptedCanFlashTransport& transport, const Variant& variant, bool accept_key = true,
                                  bool tolerate_identities = false)
{
    script_kernel_probe_timeout(transport);
    if (tolerate_identities)
    {
        script_tolerated_identity_failures(transport);
    }
    else
    {
        script_identity_queries(transport,
                                variant.security == SubaruDensoSh7058CanSecurity::RaceRomAlt ? "AE5Z500V" : "CALID");
    }
    if (variant.security == SubaruDensoSh7058CanSecurity::RaceRomAlt)
    {
        script_racerom_alt_ram(transport);
    }
    const bool use_42 = variant.security == SubaruDensoSh7058CanSecurity::RaceRom;
    script_session_selection(transport, use_42);
    script_seed_and_programming(transport, variant.expected_key, use_42, accept_key);
}

void queue_upload_b6_reply(ScriptedCanFlashTransport& transport, UploadB6Reply reply)
{
    switch (reply)
    {
    case UploadB6Reply::NoFrame:
        transport.queue_no_frame();
        return;
    case UploadB6Reply::Timeout:
        transport.queue_error(ErrorKind::Timeout, "legacy B6 timeout");
        return;
    case UploadB6Reply::Short:
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07});
        return;
    case UploadB6Reply::Malformed:
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE});
        return;
    case UploadB6Reply::WrongCanId:
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x01, 0x01});
        return;
    case UploadB6Reply::AdapterError:
        transport.queue_error(ErrorKind::Internal, "legacy stale B6 adapter error");
        return;
    case UploadB6Reply::Cancelled:
        transport.queue_error(ErrorKind::Cancelled, "legacy B6 cancellation");
        return;
    case UploadB6Reply::Disconnected:
        transport.queue_error(ErrorKind::Disconnected, "legacy B6 disconnect");
        return;
    }
}

void script_129_byte_kernel_upload_until_start(ScriptedCanFlashTransport& transport,
                                               UploadB6Reply b6_reply = UploadB6Reply::NoFrame)
{
    // Literal transcript for 128 zero bytes followed by 01. Padding expands
    // to 0x100 bytes; fixed encrypted words were derived independently from
    // the revision-59f4e442 payload algorithm.
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x34, 0x04, 0x33, 0xFF, 0x30, 0x00, 0x00, 0x01, 0x00});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x74, 0x20});

    bytes::Bytes first{0x00, 0x00, 0x07, 0xE0, 0xB6, 0xFF, 0x30, 0x00};
    for (int word = 0; word < 32; ++word)
    {
        first.insert(first.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    transport.expectWrite(first);
    queue_upload_b6_reply(transport, b6_reply);

    bytes::Bytes second{0x00, 0x00, 0x07, 0xE0, 0xB6, 0xFF, 0x30, 0x80, 0xC0, 0x41, 0xD4, 0xCA};
    for (int word = 0; word < 30; ++word)
    {
        second.insert(second.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    second.insert(second.end(), {0x42, 0x61, 0xDB, 0x2C});
    transport.expectWrite(second);
    queue_upload_b6_reply(transport, b6_reply);

    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0xFF, 0x31, 0x00});
    queue_upload_b6_reply(transport, b6_reply);
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x37});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x77});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x31, 0x01, 0x02, 0x02, 0x02});
}

void script_129_byte_kernel_upload(ScriptedCanFlashTransport& transport,
                                   bytes::Bytes final_kernel_id_response = kernel_id_response(),
                                   UploadB6Reply b6_reply = UploadB6Reply::NoFrame,
                                   bytes::Bytes start_reply = bytes::Bytes{0x71, 0x01, 0x02, 0x02, 0x02})
{
    script_129_byte_kernel_upload_until_start(transport, b6_reply);
    transport.queueRead(response(start_reply));
    transport.expectWrite(kernel_id_request());
    transport.queueRead(final_kernel_id_response);
}

void script_zero_read_pages(ScriptedCanFlashTransport& transport)
{
    for (std::uint32_t address = 0; address < kRomSize; address += kReadPageSize)
    {
        bytes::Bytes payload{0x00,
                             static_cast<bytes::Byte>(address >> 16U),
                             static_cast<bytes::Byte>(address >> 8U),
                             static_cast<bytes::Byte>(address),
                             0x04,
                             0x00};
        transport.expectWrite(beef_request(0x03, payload));
        transport.queueRead(beef_response(0x43, bytes::Bytes(kReadPageSize, bytes::Byte{0})));
    }
}

void script_raw_read_pages_with_boundary_sentinels(ScriptedCanFlashTransport& transport)
{
    constexpr std::array<bytes::Byte, 8> kFirstWireBytes{0xD3, 0x5A, 0xC7, 0x19, 0x2E, 0xF4, 0x80, 0x6B};
    constexpr std::array<bytes::Byte, 8> kLastWireBytes{0x9C, 0x31, 0xE7, 0x04, 0xB2, 0x6D, 0x58, 0xAF};
    for (std::uint32_t address = 0; address < kRomSize; address += kReadPageSize)
    {
        const bytes::Bytes payload{0x00,
                                   static_cast<bytes::Byte>(address >> 16U),
                                   static_cast<bytes::Byte>(address >> 8U),
                                   static_cast<bytes::Byte>(address),
                                   0x04,
                                   0x00};
        transport.expectWrite(beef_request(0x03, payload));
        bytes::Bytes page(kReadPageSize, bytes::Byte{0});
        if (address == 0)
        {
            std::copy(kFirstWireBytes.begin(), kFirstWireBytes.end(), page.begin());
        }
        if (address + kReadPageSize == kRomSize)
        {
            std::copy(kLastWireBytes.begin(), kLastWireBytes.end(), page.end() - kLastWireBytes.size());
        }
        transport.queueRead(beef_response(0x43, page));
    }
}

void script_crc(ScriptedCanFlashTransport& transport, const BlockFixture& block, std::uint32_t crc,
                std::optional<Result<std::optional<bytes::Bytes>>> stale = std::nullopt)
{
    bytes::Bytes payload = be32(block.start);
    payload.push_back(0x00);
    payload.push_back(static_cast<bytes::Byte>(block.length >> 16U));
    payload.push_back(static_cast<bytes::Byte>(block.length >> 8U));
    payload.push_back(static_cast<bytes::Byte>(block.length));
    transport.expectWrite(beef_request(0x02, payload));
    transport.queueRead(beef_response(0x42, be32(crc)));
    if (!stale.has_value())
    {
        transport.queue_no_frame();
    }
    else if (!stale->has_value())
    {
        transport.queue_error(stale->error().kind, stale->error().detail);
    }
    else if (!stale->value().has_value())
    {
        transport.queue_no_frame();
    }
    else
    {
        transport.queueRead(***stale);
    }
}

void script_compare(ScriptedCanFlashTransport& transport, std::optional<std::size_t> mismatch,
                    bool a5_block_matches = false)
{
    for (std::size_t index = 0; index < kBlocks.size(); ++index)
    {
        std::uint32_t crc = kBlocks[index].zero_crc;
        if (index == 8 && a5_block_matches)
        {
            crc = 0xAAA0B108;
        }
        if (mismatch == index)
        {
            crc ^= 1U;
        }
        script_crc(transport, kBlocks[index], crc);
    }
}

void script_flash_init(ScriptedCanFlashTransport& transport, bool test_write)
{
    transport.expectWrite(beef_request(0x05));
    transport.queueRead(beef_response(0x45, bytes::Bytes{0x00, 0x00, 0x02, 0x00}));
    transport.expectWrite(beef_request(0x06));
    transport.queueRead(beef_response(0x46, bytes::Bytes{0x00, 0x00, 0x10, 0x00}));
    transport.expectWrite(beef_request(test_write ? 0x21 : 0x20));
    transport.queueRead(beef_response(test_write ? 0x61 : 0x60));
}

void script_flash_block(ScriptedCanFlashTransport& transport, const BlockFixture& block, bytes::Byte fill,
                        bool test_write)
{
    transport.expectWrite(beef_request(0x04));
    transport.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
    transport.expectWrite(beef_request(0x25, be32(block.start)));
    transport.queueRead(beef_response(0x65));
    bytes::Bytes chunk(kWriteChunkSize, fill);
    for (std::uint32_t address = block.start; address < block.start + block.length; address += kWriteChunkSize)
    {
        bytes::Bytes write_payload = be32(address);
        write_payload.insert(write_payload.end(), chunk.begin(), chunk.end());
        transport.expectWrite(beef_request(0x22, write_payload));
        transport.queueRead(beef_response(0x62));
        if ((address + kWriteChunkSize - block.start) % kCommitSize == 0)
        {
            const std::uint32_t commit_address = address + kWriteChunkSize - kCommitSize;
            bytes::Bytes commit = be32(commit_address);
            const bytes::Bytes size = be16(kCommitSize);
            commit.insert(commit.end(), size.begin(), size.end());
            const bytes::Bytes crc = be32(fill == 0xA5 ? 0x958BA140U : 0xF722EF49U);
            commit.insert(commit.end(), crc.begin(), crc.end());
            transport.expectWrite(beef_request(test_write ? 0x23 : 0x24, commit));
            transport.queueRead(beef_response(test_write ? 0x63 : 0x64));
        }
    }
}

bool has_log(const RecordingEventSink& events, LogLevel level, std::string_view needle)
{
    return std::any_of(events.logs.begin(), events.logs.end(), [level, needle](const auto& record)
                       { return record.first == level && record.second.find(needle) != std::string::npos; });
}

std::vector<std::string> logs_starting_with(const RecordingEventSink& events, std::string_view prefix)
{
    std::vector<std::string> selected;
    for (const auto& [level, message] : events.logs)
    {
        static_cast<void>(level);
        if (message.starts_with(prefix))
        {
            selected.push_back(message);
        }
    }
    return selected;
}

TEST(SubaruDensoSh7058CanExecutor, TransportSetupUsesExactPetrolIsoConfiguration)
{
    SubaruDensoSh7058CanExecutor executor;
    for (const Variant& variant : kVariants)
    {
        auto plan = plan_for(variant, FlashOperation::Read);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto setup = executor.transport_setup(*plan);
        ASSERT_TRUE(setup.has_value()) << setup.error().detail;
        EXPECT_EQ(setup->bitrate, 500000);
        EXPECT_EQ(setup->request_id, 0x7E0U);
        EXPECT_EQ(setup->response_id, 0x7E8U);
        EXPECT_FALSE(setup->extended_id);
    }
}

TEST(SubaruDensoSh7058CanExecutor, BoundAttemptResetsBeforeConfiguringAndOpening)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto transport = std::make_unique<RecordingCanTransport>();
    RecordingCanTransport *observed = transport.get();
    ToggleCancellation cancellation;
    observed->cancellation_on_open = &cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto attempt =
        bind_flash_attempt(std::move(*plan), std::make_unique<SubaruDensoSh7058CanExecutor>(), std::move(transport));
    const auto result = attempt->run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_THAT(observed->lifecycle, ElementsAre("reset_connection", "configure", "open", "close"));
}

TEST(SubaruDensoSh7058CanExecutor, StartupCancellationBeforeResetTouchesNoLifecycleOperation)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto transport = std::make_unique<RecordingCanTransport>();
    RecordingCanTransport *observed = transport.get();
    ToggleCancellation cancellation;
    cancellation.cancel();
    RecordingClock clock;
    RecordingEventSink events;

    auto attempt =
        bind_flash_attempt(std::move(*plan), std::make_unique<SubaruDensoSh7058CanExecutor>(), std::move(transport));
    const auto result = attempt->run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(observed->lifecycle.empty());
}

TEST(SubaruDensoSh7058CanExecutor, StartupCancellationAfterResetSkipsConfigureOpenAndClose)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto transport = std::make_unique<RecordingCanTransport>();
    RecordingCanTransport *observed = transport.get();
    ToggleCancellation cancellation;
    observed->cancellation_on_reset = &cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto attempt =
        bind_flash_attempt(std::move(*plan), std::make_unique<SubaruDensoSh7058CanExecutor>(), std::move(transport));
    const auto result = attempt->run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_THAT(observed->lifecycle, ElementsAre("reset_connection"));
}

TEST(SubaruDensoSh7058CanExecutor, StartupCancellationDuringConfigureSkipsOpenAndClose)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto transport = std::make_unique<RecordingCanTransport>();
    RecordingCanTransport *observed = transport.get();
    ToggleCancellation cancellation;
    observed->cancellation_on_configure = &cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto attempt =
        bind_flash_attempt(std::move(*plan), std::make_unique<SubaruDensoSh7058CanExecutor>(), std::move(transport));
    const auto result = attempt->run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_THAT(observed->lifecycle, ElementsAre("reset_connection", "configure"));
}

TEST(SubaruDensoSh7058CanExecutor, StartupResetFailurePropagatesWithoutConfigureOpenOrClose)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto transport = std::make_unique<RecordingCanTransport>();
    RecordingCanTransport *observed = transport.get();
    observed->reset_result = fail(ErrorKind::Internal, "petrol reset marker");
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto attempt =
        bind_flash_attempt(std::move(*plan), std::make_unique<SubaruDensoSh7058CanExecutor>(), std::move(transport));
    const auto result = attempt->run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_EQ(result.error().detail, "petrol reset marker");
    EXPECT_THAT(observed->lifecycle, ElementsAre("reset_connection"));
}

TEST(SubaruDensoSh7058CanExecutor, AllFiveValidatedEnumsProduceTheirFixedSecurityVector)
{
    for (const Variant& variant : kVariants)
    {
        SCOPED_TRACE(variant.protocol);
        auto plan = plan_for(variant, FlashOperation::Read);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport, variant, false);
        NeverCancelled cancellation;
        RecordingClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_TRUE(has_log(events, LogLevel::Info,
                            variant.security == SubaruDensoSh7058CanSecurity::RaceRom ? "Using EcuTek RaceRom RSA algo"
                            : variant.security == SubaruDensoSh7058CanSecurity::RaceRomAlt ||
                                    variant.security == SubaruDensoSh7058CanSecurity::EcuTek
                                ? "Using EcuTek seed key algo"
                            : variant.security == SubaruDensoSh7058CanSecurity::Cobb ? "Using COBB seed key algo"
                                                                                     : "Using stock seed key algo"));
    }
}

TEST(SubaruDensoSh7058CanExecutor, AlreadyRunningKernelReadsFirstAndLastBoundaryWithExactLogsAndProgress)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport.scripted);
    script_zero_read_pages(transport.scripted);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), kRomSize);
    EXPECT_TRUE(std::all_of(result->read_bytes->begin(), result->read_bytes->end(),
                            [](bytes::Byte byte) { return byte == 0; }));
    EXPECT_EQ(result->rom_id, std::nullopt);
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    ASSERT_EQ(transport.writes.size(), 1025U);
    EXPECT_EQ(transport.writes[1],
              (bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00}));
    EXPECT_EQ(transport.writes.back(),
              (bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x0F, 0xFC, 0x00, 0x04, 0x00}));
    EXPECT_THAT(events.notices, ElementsAre("Reading ROM, please wait..."));
    const auto read_logs = logs_starting_with(events, "Kernel read addr:");
    ASSERT_EQ(read_logs.size(), 1024U);
    EXPECT_EQ(read_logs.front(), "Kernel read addr: 0x00000000 length: 0x00000400, 1024000 B/s      2 s");
    EXPECT_EQ(read_logs.back(), "Kernel read addr: 0x000FFC00 length: 0x00000400, 1024000 B/s      1 s");
    ASSERT_EQ(events.phase_progress_calls.size(), 1028U);
    EXPECT_EQ(events.phase_progress_calls.front().phase_name, "Kernel");
    EXPECT_EQ(events.phase_progress_calls.front().done, 0);
    EXPECT_EQ(events.phase_progress_calls[1].done, 1);
    EXPECT_EQ(events.phase_progress_calls[2].phase_name, "Read");
    EXPECT_EQ(events.phase_progress_calls[2].done, 0);
    EXPECT_EQ(events.phase_progress_calls.back().phase_name, "Read");
    EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(kRomSize));
}

TEST(SubaruDensoSh7058CanExecutor, ReadKeepsRawBeefBoundaryPayloadsForEveryPetrolVariant)
{
    SubaruDensoSh7058CanExecutor executor;
    NeverCancelled cancellation;
    for (const Variant& variant : kVariants)
    {
        SCOPED_TRACE(variant.protocol);
        auto plan = plan_for(variant, FlashOperation::Read);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_raw_read_pages_with_boundary_sentinels(transport);
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << result.error().detail;
        ASSERT_TRUE(result->read_bytes.has_value());
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(8),
                    ElementsAre(0xD3, 0x5A, 0xC7, 0x19, 0x2E, 0xF4, 0x80, 0x6B));
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).last(8),
                    ElementsAre(0x9C, 0x31, 0xE7, 0x04, 0xB2, 0x6D, 0x58, 0xAF));
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh7058CanExecutor, ProbeTimeoutUploadsLiteral129ByteKernelThenReadsAndRendersRomId)
{
    bytes::Bytes kernel_data(129, bytes::Byte{0});
    kernel_data.back() = 0x01;
    auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_bootloader_connection(transport.scripted, kVariants.front());
    script_129_byte_kernel_upload(transport.scripted);
    script_zero_read_pages(transport.scripted);
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), kRomSize);
    EXPECT_EQ(result->rom_id, "CALID_123456789A_");
    EXPECT_THAT(events.notices, ElementsAre("Preparing, please wait...", "Reading ROM, please wait..."));
    EXPECT_TRUE(has_log(events, LogLevel::Info, "VIN: VIN"));
    EXPECT_TRUE(has_log(events, LogLevel::Info, "CVN: 1234"));
    EXPECT_TRUE(has_log(events, LogLevel::Info, "Kernel ID: KID"));
    EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 50ms), 12);
    EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 100ms), 1);
    EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 200ms), 2);
    EXPECT_THAT(transport.read_timeouts, Contains(10ms));
}

TEST(SubaruDensoSh7058CanExecutor, UploadB6ShortMalformedWrongIdAndAdapterRepliesAreDiscarded)
{
    const std::array<UploadB6Reply, 6> ignored_replies{UploadB6Reply::NoFrame,    UploadB6Reply::Timeout,
                                                       UploadB6Reply::Short,      UploadB6Reply::Malformed,
                                                       UploadB6Reply::WrongCanId, UploadB6Reply::AdapterError};
    for (const UploadB6Reply b6_reply : ignored_replies)
    {
        SCOPED_TRACE(static_cast<int>(b6_reply));
        bytes::Bytes kernel_data(129, bytes::Byte{0});
        kernel_data.back() = 0x01;
        auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport, kVariants.front());
        // Stop at the strict post-upload kernel probe so the test proves all
        // three B6 reads were discarded before that probe, without needing a
        // 1 MiB read transcript in each reply variant.
        script_129_byte_kernel_upload(transport, bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE}, b6_reply);
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh7058CanExecutor, UploadB6CancellationAndDisconnectArePropagated)
{
    for (const auto [b6_reply, expected] : {std::pair{UploadB6Reply::Cancelled, ErrorKind::Cancelled},
                                            std::pair{UploadB6Reply::Disconnected, ErrorKind::Disconnected}})
    {
        SCOPED_TRACE(static_cast<int>(b6_reply));
        bytes::Bytes kernel_data(129, bytes::Byte{0});
        kernel_data.back() = 0x01;
        auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport, kVariants.front());
        script_129_byte_kernel_upload(transport, bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE}, b6_reply);
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, expected);
    }
}

TEST(SubaruDensoSh7058CanExecutor, KernelStartAcceptsServiceOnlyAndFullEchoBeforeStrictPostUploadProbe)
{
    const std::array<bytes::Bytes, 2> accepted{{bytes::Bytes{0x71}, bytes::Bytes{0x71, 0x01, 0x02, 0x02, 0x02}}};
    for (const bytes::Bytes& start_reply : accepted)
    {
        SCOPED_TRACE(bytes::toHex(start_reply));
        bytes::Bytes kernel_data(129, bytes::Byte{0});
        kernel_data.back() = 0x01;
        auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport, kVariants.front());
        script_129_byte_kernel_upload(transport, bytes::Bytes{0x00, 0x00, 0x07}, UploadB6Reply::NoFrame, start_reply);
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh7058CanExecutor, KernelStartRejectsWrongSidShortTimeoutCancellationAndDisconnect)
{
    struct StartCase
    {
        std::string_view name;
        std::optional<bytes::Bytes> frame;
        std::optional<ErrorKind> error;
        ErrorKind expected;
    };
    const std::array<StartCase, 5> cases{{
        {"wrong-sid", response(bytes::Bytes{0x70}), std::nullopt, ErrorKind::BadResponse},
        {"short", bytes::Bytes{0x00, 0x00, 0x07}, std::nullopt, ErrorKind::BadResponse},
        {"timeout", std::nullopt, ErrorKind::Timeout, ErrorKind::Timeout},
        {"cancelled", std::nullopt, ErrorKind::Cancelled, ErrorKind::Cancelled},
        {"disconnected", std::nullopt, ErrorKind::Disconnected, ErrorKind::Disconnected},
    }};
    for (const StartCase& start : cases)
    {
        SCOPED_TRACE(start.name);
        bytes::Bytes kernel_data(129, bytes::Byte{0});
        kernel_data.back() = 0x01;
        auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport, kVariants.front());
        script_129_byte_kernel_upload_until_start(transport);
        if (start.error.has_value())
        {
            transport.queue_error(*start.error, "kernel-start read outcome");
        }
        else
        {
            transport.queueRead(*start.frame);
        }
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, start.expected);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh7058CanExecutor, IdentityAndVendorFailuresRemainTolerantUntilStrictSecurity)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_bootloader_connection(transport, kVariants.front(), false, true);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(has_log(events, LogLevel::Error, "No valid response from ECU"));
    EXPECT_TRUE(has_log(events, LogLevel::Info, "Sending seed key"));
}

TEST(SubaruDensoSh7058CanExecutor, ShortAndWrongIdInitialKernelProbesFallBackToBootloaderNegotiation)
{
    const std::array<bytes::Bytes, 2> probe_replies{bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE},
                                                    bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x01, 0x01}};
    for (const bytes::Bytes& probe_reply : probe_replies)
    {
        SCOPED_TRACE(bytes::toHex(probe_reply));
        auto plan = plan_for(kVariants.front(), FlashOperation::Read);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);

        // The legacy initial probe logs a malformed/wrong kernel reply and
        // continues with the ECU identity/session sequence. The strict
        // security exchange below makes fallback observable without a full
        // upload/read transcript.
        transport.expectWrite(kernel_id_request());
        transport.queueRead(probe_reply);
        script_identity_queries(transport);
        script_session_selection(transport);
        transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x01});
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x67, 0x01, 0x11, 0x22, 0x33, 0x44});
        transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x02, 0x35, 0xB6, 0x83, 0xBF});
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x7F, 0x27, 0x35});

        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_TRUE(has_log(events, LogLevel::Info, "Requesting ECU ID"));
        EXPECT_TRUE(has_log(events, LogLevel::Info, "Sending seed key"));
    }
}

TEST(SubaruDensoSh7058CanExecutor, TestWriteUsesDisableEraseBufferAndValidateWithoutCommit)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::TestWrite);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport.scripted);
    script_compare(transport.scripted, 0U);
    script_flash_init(transport.scripted, true);
    script_flash_block(transport.scripted, kBlocks[0], 0x00, true);
    script_compare(transport.scripted, 0U);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::TestWrite);
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    EXPECT_THAT(events.notices, ElementsAre("Writing ROM, please wait..."));
    EXPECT_TRUE(has_log(events, LogLevel::Info, "Test write mode on, no actual flash write is performed"));
    EXPECT_TRUE(has_log(events, LogLevel::Info, "*** Test write PASS, it's ok to perform actual write! ***"));
    const auto validate = beef_request(0x23, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0xF7, 0x22, 0xEF, 0x49});
    EXPECT_NE(std::find(transport.writes.begin(), transport.writes.end(), validate), transport.writes.end());
    EXPECT_EQ(std::count_if(transport.writes.begin(), transport.writes.end(),
                            [](const bytes::Bytes& wire) { return wire.size() > 8 && wire[8] == 0x24; }),
              0);
}

TEST(SubaruDensoSh7058CanExecutor, UnchangedWriteSkipsFlashAndStillCompletesFourOrderedPhases)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Write);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare(transport, std::nullopt);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(has_log(events, LogLevel::Info,
                        "*** Compare results no difference between ROM and ECU data, no flashing needed! ***"));
    ASSERT_EQ(events.phase_progress_calls.size(), 22U);
    EXPECT_EQ(events.phase_progress_calls[0], (RecordedPhaseProgress{"Kernel", 1, 4, 0, 1}));
    EXPECT_EQ(events.phase_progress_calls[1], (RecordedPhaseProgress{"Kernel", 1, 4, 1, 1}));
    EXPECT_EQ(events.phase_progress_calls[18], (RecordedPhaseProgress{"Compare", 2, 4, 16, 16}));
    EXPECT_EQ(events.phase_progress_calls[19], (RecordedPhaseProgress{"Write", 3, 4, 0, 0}));
    EXPECT_EQ(events.phase_progress_calls[20], (RecordedPhaseProgress{"Complete", 4, 4, 0, 1}));
    EXPECT_EQ(events.phase_progress_calls[21], (RecordedPhaseProgress{"Complete", 4, 4, 1, 1}));
}

TEST(SubaruDensoSh7058CanExecutor, SmallChangedWriteErasesProgramsCommitsVerifiesAndPinsCorrectedSpeedLog)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Write);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport.scripted);
    script_compare(transport.scripted, 0U);
    script_flash_init(transport.scripted, false);
    script_flash_block(transport.scripted, kBlocks[0], 0x00, false);
    script_compare(transport.scripted, std::nullopt);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    EXPECT_FALSE(has_log(events, LogLevel::Error, "*** ERROR IN FLASH PROCESS ***"));
    EXPECT_THAT(logs_starting_with(events, "Write flash buffer:"),
                ElementsAre("Write flash buffer: 0x00000000 (0% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000200 (12% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000400 (25% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000600 (37% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000800 (50% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000A00 (62% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000C00 (75% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000E00 (87% - 512000 B/s, ~ 1 s)"));
    ASSERT_EQ(events.phase_progress_calls.size(), 31U);
    EXPECT_EQ(events.phase_progress_calls[19], (RecordedPhaseProgress{"Write", 3, 4, 0, 0x1000}));
    EXPECT_EQ(events.phase_progress_calls[27], (RecordedPhaseProgress{"Write", 3, 4, 0x0FFF, 0x1000}));
    EXPECT_EQ(events.phase_progress_calls[28], (RecordedPhaseProgress{"Write", 3, 4, 0x1000, 0x1000}));
    EXPECT_EQ(events.phase_progress_calls.back(), (RecordedPhaseProgress{"Complete", 4, 4, 1, 1}));
}

TEST(SubaruDensoSh7058CanExecutor, NonzeroLargeBlockUsesAllRepeatedCommitWindowsAndChangedByteProgress)
{
    bytes::Bytes image(kRomSize, bytes::Byte{0});
    std::fill(image.begin() + 0x8000, image.begin() + 0x20000, bytes::Byte{0xA5});
    auto plan = plan_for(kVariants.back(), FlashOperation::Write, std::move(image));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport.scripted);
    script_compare(transport.scripted, std::nullopt);
    script_flash_init(transport.scripted, false);
    script_flash_block(transport.scripted, kBlocks[8], 0xA5, false);
    script_compare(transport.scripted, std::nullopt, true);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    std::vector<bytes::Bytes> commits;
    for (const bytes::Bytes& wire : transport.writes)
    {
        if (wire.size() > 8 && wire[8] == 0x24)
        {
            commits.push_back(wire);
        }
    }
    ASSERT_EQ(commits.size(), 24U);
    EXPECT_EQ(commits.front(), (bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x0B, 0x24, 0x00, 0x00, 0x80,
                                             0x00, 0x10, 0x00, 0x95, 0x8B, 0xA1, 0x40}));
    EXPECT_EQ(commits.back(), (bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x0B, 0x24, 0x00, 0x01, 0xF0,
                                            0x00, 0x10, 0x00, 0x95, 0x8B, 0xA1, 0x40}));
    ASSERT_EQ(events.phase_progress_calls.size(), 215U);
    EXPECT_EQ(events.phase_progress_calls[19], (RecordedPhaseProgress{"Write", 3, 4, 0, 0x18000}));
    EXPECT_EQ(events.phase_progress_calls[211], (RecordedPhaseProgress{"Write", 3, 4, 0x17FFF, 0x18000}));
    EXPECT_EQ(events.phase_progress_calls[212], (RecordedPhaseProgress{"Write", 3, 4, 0x18000, 0x18000}));
}

TEST(SubaruDensoSh7058CanExecutor, CrcStaleDrainPreservesToleranceButPropagatesCancellationAndDisconnect)
{
    for (const std::optional<ErrorKind> terminal :
         {std::optional<ErrorKind>{}, std::optional<ErrorKind>{ErrorKind::Cancelled},
          std::optional<ErrorKind>{ErrorKind::Disconnected}})
    {
        auto plan = plan_for(kVariants.front(), FlashOperation::Write);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        if (!terminal.has_value())
        {
            script_crc(transport, kBlocks[0], kBlocks[0].zero_crc,
                       Result<std::optional<bytes::Bytes>>{std::optional<bytes::Bytes>{bytes::Bytes{0x12, 0x34}}});
            for (std::size_t index = 1; index < kBlocks.size(); ++index)
            {
                script_crc(transport, kBlocks[index], kBlocks[index].zero_crc);
            }
        }
        else
        {
            script_crc(transport, kBlocks[0], kBlocks[0].zero_crc,
                       Result<std::optional<bytes::Bytes>>{fail(*terminal, "stale drain terminal")});
        }
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        if (!terminal.has_value())
        {
            ASSERT_TRUE(result.has_value()) << result.error().detail;
        }
        else
        {
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().kind, *terminal);
        }
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh7058CanExecutor, RejectsInvalidSecurityPlanBeforeTransportInteraction)
{
    FlashPlanFields fields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruDensoSh7058Can,
        .transport = TransportKind::CanIso15765,
        .target_id = "sub_ecu_denso_sh7058_can",
        .mcu_name = "SH7058",
        .transfer_region = {0, kRomSize},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = kernel(),
        .family_plan = SubaruDensoSh7058CanPlan{.security = SubaruDensoSh7058CanSecurity::Cobb},
        .confirmations = {},
    };
    auto plan = validate_and_build(std::move(fields));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanExecutor executor;
    ScriptedCanFlashTransport transport;
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto setup = executor.transport_setup(*plan);
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(setup.has_value());
    EXPECT_EQ(setup.error().kind, ErrorKind::InvalidConfig);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

TEST(SubaruDensoSh7058CanExecutor, MalformedNegativeTimeoutAndDisconnectRepliesAreTyped)
{
    struct ErrorCase
    {
        std::string_view name;
        Result<std::optional<bytes::Bytes>> reply;
        ErrorKind expected;
    };
    const std::array<ErrorCase, 3> cases{{
        {"negative", std::optional<bytes::Bytes>{response(bytes::Bytes{0x7F, 0x27, 0x35})}, ErrorKind::BadResponse},
        {"timeout", std::optional<bytes::Bytes>{}, ErrorKind::Timeout},
        {"disconnect", fail(ErrorKind::Disconnected, "adapter disconnected"), ErrorKind::Disconnected},
    }};
    for (const ErrorCase& test_case : cases)
    {
        SCOPED_TRACE(test_case.name);
        auto plan = plan_for(kVariants.front(), FlashOperation::Read);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        if (test_case.name == "negative" || test_case.name == "timeout")
        {
            script_kernel_probe_timeout(transport);
            script_identity_queries(transport);
            script_session_selection(transport);
            transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x01});
        }
        else
        {
            transport.expectWrite(kernel_id_request());
        }
        if (!test_case.reply.has_value())
        {
            transport.queue_error(test_case.reply.error().kind, test_case.reply.error().detail);
        }
        else if (!test_case.reply->has_value())
        {
            transport.queue_no_frame();
        }
        else
        {
            transport.queueRead(**test_case.reply);
        }
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, test_case.expected);
    }
}

TEST(SubaruDensoSh7058CanExecutor, MalformedKernelIdAfterUploadIsStrictlyRejected)
{
    bytes::Bytes kernel_data(129, bytes::Byte{0});
    kernel_data.back() = 0x01;
    auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_bootloader_connection(transport, kVariants.front());
    script_129_byte_kernel_upload(transport, bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE});

    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh7058CanExecutor, StrictUdsExchangeReadsResponsePendingWithoutResending)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_probe_timeout(transport.scripted);
    script_identity_queries(transport.scripted);
    script_session_selection(transport.scripted);
    transport.scripted.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x01});
    transport.scripted.queueRead(response(bytes::Bytes{0x7F, 0x27, 0x78}));
    transport.scripted.queueRead(response(bytes::Bytes{0x67, 0x01, 0x11, 0x22, 0x33, 0x44}));
    transport.scripted.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x02, 0x35, 0xB6, 0x83, 0xBF});
    transport.scripted.queueRead(response(bytes::Bytes{0x7F, 0x27, 0x35}));

    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    EXPECT_EQ(
        std::count(transport.writes.begin(), transport.writes.end(), bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x01}),
        1U);
    EXPECT_THAT(transport.read_timeouts, Contains(3000ms));
}

TEST(SubaruDensoSh7058CanExecutor, ProprietaryBeefWrongOpcodeAndCanIdAreTypedBadResponses)
{
    // BEEF exchanges bypass UDS, so both the proprietary opcode and the
    // ISO envelope response id must be validated by the executor/channel.
    {
        auto plan = plan_for(kVariants.front(), FlashOperation::Read);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        const bytes::Bytes payload{0x00, 0x00, 0x00, 0x00, 0x04, 0x00};
        transport.expectWrite(beef_request(0x03, payload));
        transport.queueRead(beef_response(0x44, bytes::Bytes(kReadPageSize, bytes::Byte{0x00})));
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }

    {
        bytes::Bytes kernel_data(129, bytes::Byte{0});
        kernel_data.back() = 0x01;
        auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport, kVariants.front());
        script_129_byte_kernel_upload(
            transport, bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x04, 0x41, 0x4B, 0x49, 0x44});
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh7058CanExecutor, CancellationInterruptsProbeUploadReadCrcEraseWriteAndCommitBoundaries)
{
    // Probe boundary: an already-cancelled operation performs no I/O.
    {
        auto plan = plan_for(kVariants.front(), FlashOperation::Read);
        ASSERT_TRUE(plan.has_value());
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        ToggleCancellation cancellation;
        cancellation.cancel();
        FakeClock clock;
        RecordingEventSink events;
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_EQ(transport.writesConsumed(), 0U);
    }

    // Upload boundary: cancellation after the first 128-byte transfer keeps
    // the following transfer, exit and kernel probe unconsumed.
    {
        auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, bytes::Bytes(129, 0));
        ASSERT_TRUE(plan.has_value());
        SubaruDensoSh7058CanExecutor executor;
        RecordingCanTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport.scripted, kVariants.front());
        transport.scripted.expectWrite(
            bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x34, 0x04, 0x33, 0xFF, 0x30, 0x00, 0x00, 0x01, 0x00});
        transport.scripted.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x74, 0x20});
        bytes::Bytes first{0x00, 0x00, 0x07, 0xE0, 0xB6, 0xFF, 0x30, 0x00};
        for (int word = 0; word < 32; ++word)
        {
            first.insert(first.end(), {0xE7, 0xE2, 0x14, 0x30});
        }
        transport.scripted.expectWrite(first);
        transport.scripted.queue_no_frame();
        ToggleCancellation cancellation;
        transport.cancellation_to_trigger = &cancellation;
        transport.cancel_prefix = {0x00, 0x00, 0x07, 0xE0, 0xB6};
        FakeClock clock;
        RecordingEventSink events;
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    }

    // Read loop boundary after the first page.
    {
        auto plan = plan_for(kVariants.front(), FlashOperation::Read);
        ASSERT_TRUE(plan.has_value());
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        transport.expectWrite(
            bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00});
        bytes::Bytes page;
        for (int word = 0; word < 256; ++word)
        {
            page.insert(page.end(), {0xE7, 0xE2, 0x14, 0x30});
        }
        transport.queueRead(beef_response(0x43, page));
        ToggleCancellation cancellation;
        PhaseCancellingEventSink events(cancellation, "Read", 0x400);
        FakeClock clock;
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    }

    // CRC boundary after the first completed comparison.
    {
        auto plan = plan_for(kVariants.front(), FlashOperation::Write);
        ASSERT_TRUE(plan.has_value());
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_crc(transport, kBlocks[0], kBlocks[0].zero_crc);
        ToggleCancellation cancellation;
        PhaseCancellingEventSink events(cancellation, "Compare", 1);
        FakeClock clock;
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    }

    // Erase/write loop boundary after the erase acknowledgement preserves
    // the legacy recovery notice and sends no buffer bytes.
    {
        auto plan = plan_for(kVariants.front(), FlashOperation::Write);
        ASSERT_TRUE(plan.has_value());
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, 0U);
        script_flash_init(transport, false);
        transport.expectWrite(beef_request(0x04));
        transport.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
        transport.expectWrite(beef_request(0x25, bytes::Bytes{0x00, 0x00, 0x00, 0x00}));
        transport.queueRead(beef_response(0x65));
        ToggleCancellation cancellation;
        EraseCancellingEventSink events(cancellation);
        FakeClock clock;
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_TRUE(has_log(events, LogLevel::Error, "Reflash error! Do not panic, do not reset the ECU immediately"));
    }

    // Write boundary after the first buffer write.
    {
        auto plan = plan_for(kVariants.front(), FlashOperation::Write);
        ASSERT_TRUE(plan.has_value());
        SubaruDensoSh7058CanExecutor executor;
        RecordingCanTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport.scripted);
        script_compare(transport.scripted, 0U);
        script_flash_init(transport.scripted, false);
        transport.scripted.expectWrite(beef_request(0x04));
        transport.scripted.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
        transport.scripted.expectWrite(beef_request(0x25, bytes::Bytes{0x00, 0x00, 0x00, 0x00}));
        transport.scripted.queueRead(beef_response(0x65));
        transport.scripted.expectWrite(beef_request(0x22, bytes::Bytes(516, 0)));
        // Replace the first four bytes with the literal address.
        ToggleCancellation cancellation;
        transport.cancellation_to_trigger = &cancellation;
        transport.cancel_prefix = {0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x02, 0x05, 0x22};
        FakeClock clock;
        RecordingEventSink events;
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    }

    // Commit boundary: cancellation on the final incomplete progress event
    // is observed before the commit command.
    {
        auto plan = plan_for(kVariants.front(), FlashOperation::Write);
        ASSERT_TRUE(plan.has_value());
        SubaruDensoSh7058CanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, 0U);
        script_flash_init(transport, false);
        transport.expectWrite(beef_request(0x04));
        transport.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
        transport.expectWrite(beef_request(0x25, bytes::Bytes{0x00, 0x00, 0x00, 0x00}));
        transport.queueRead(beef_response(0x65));
        bytes::Bytes zeros(0x200, 0);
        for (std::uint32_t address = 0; address < 0x1000; address += 0x200)
        {
            bytes::Bytes payload = be32(address);
            payload.insert(payload.end(), zeros.begin(), zeros.end());
            transport.expectWrite(beef_request(0x22, payload));
            transport.queueRead(beef_response(0x62));
        }
        ToggleCancellation cancellation;
        PhaseCancellingEventSink events(cancellation, "Write", 0x0FFF);
        FakeClock clock;
        auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    }
}

} // namespace
} // namespace fastecu::flash
