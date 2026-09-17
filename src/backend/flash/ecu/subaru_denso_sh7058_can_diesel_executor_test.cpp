#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"
#include "src/backend/flash/flash_executor.h"
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

struct DieselVariant
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t rom_size;
    std::uint32_t kernel_address;
};

constexpr std::array<DieselVariant, 2> kVariants{{
    {"sub_ecu_denso_sh7058_can_diesel", "SH7058d", 0x00100000, 0xFFFF4000},
    {"sub_ecu_denso_sh7059_can_diesel", "SH7059d", 0x00180000, 0xFFFEE000},
}};

constexpr std::uint32_t kReadPageSize = 0x400;
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitSize = 0x1000;

struct BlockFixture
{
    std::uint32_t start;
    std::uint32_t length;
    std::uint32_t zero_crc;
};

constexpr std::array<BlockFixture, 16> kSh7058Blocks{{
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

constexpr std::array<BlockFixture, 16> kSh7059Blocks{{
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
    {0x00080000, 0x00040000, 0xE1C3FA09},
    {0x000C0000, 0x00040000, 0xE1C3FA09},
    {0x00100000, 0x00040000, 0xE1C3FA09},
    {0x00140000, 0x00040000, 0xE1C3FA09},
}};

const std::array<BlockFixture, 16>& blocks_for(const DieselVariant& variant)
{
    return variant.mcu == "SH7058d" ? kSh7058Blocks : kSh7059Blocks;
}

KernelImage kernel_for(const DieselVariant& variant, bytes::Bytes data = {0x01, 0x02, 0x03, 0x04})
{
    return {.id = std::string(variant.protocol) + "-kernel",
            .load_address = variant.kernel_address,
            .bytes = std::move(data)};
}

Result<FlashPlan> plan_for(const DieselVariant& variant, FlashOperation operation, bytes::Bytes image = {},
                           bytes::Bytes kernel_data = {0x01, 0x02, 0x03, 0x04})
{
    std::optional<bytes::Bytes> selected_image;
    if (operation != FlashOperation::Read)
    {
        if (image.empty())
        {
            image.assign(variant.rom_size, bytes::Byte{0});
        }
        selected_image = std::move(image);
    }
    return build_subaru_denso_sh7058_can_diesel_plan(operation, variant.protocol, variant.mcu,
                                                     std::move(selected_image),
                                                     kernel_for(variant, std::move(kernel_data)));
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
    return {static_cast<bytes::Byte>(value >> 8U), static_cast<bytes::Byte>(value)};
}

bytes::Bytes be32(std::uint32_t value)
{
    return {static_cast<bytes::Byte>(value >> 24U), static_cast<bytes::Byte>(value >> 16U),
            static_cast<bytes::Byte>(value >> 8U), static_cast<bytes::Byte>(value)};
}

bytes::Bytes u24(std::uint32_t value)
{
    return {static_cast<bytes::Byte>(value >> 16U), static_cast<bytes::Byte>(value >> 8U),
            static_cast<bytes::Byte>(value)};
}

bytes::Bytes beef_request(bytes::Byte opcode, bytes::ByteView payload = {})
{
    bytes::Bytes pdu{0xBE, 0xEF, static_cast<bytes::Byte>((payload.size() + 1U) >> 8U),
                     static_cast<bytes::Byte>(payload.size() + 1U), opcode};
    pdu.insert(pdu.end(), payload.begin(), payload.end());
    return request(pdu);
}

bytes::Bytes beef_response(bytes::Byte opcode, bytes::ByteView payload = {})
{
    bytes::Bytes pdu{0xBE, 0xEF, static_cast<bytes::Byte>((payload.size() + 1U) >> 8U),
                     static_cast<bytes::Byte>(payload.size() + 1U), opcode};
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
        if (timeline != nullptr)
        {
            timeline->push_back(std::format("sleep:{}", duration.count()));
        }
        if (cancel_during_sleep != nullptr)
        {
            cancel_during_sleep->cancel();
        }
        return FakeClock::sleep(duration, cancellation);
    }

    std::vector<std::chrono::milliseconds> sleeps;
    ToggleCancellation *cancel_during_sleep = nullptr;
    std::vector<std::string> *timeline = nullptr;
};

class RecordingCanTransport final : public ICanFlashTransport
{
  public:
    Status reset_connection() override
    {
        lifecycle.push_back("reset_connection");
        if (timeline != nullptr)
        {
            timeline->push_back("reset_connection");
        }
        return reset_result;
    }
    Status configure(const Iso15765Config& config) override
    {
        lifecycle.push_back("configure");
        if (timeline != nullptr)
        {
            timeline->push_back("configure");
        }
        return scripted.configure(config);
    }
    Status open() override
    {
        lifecycle.push_back("open");
        if (timeline != nullptr)
        {
            timeline->push_back("open");
        }
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
        if (timeline != nullptr)
        {
            timeline->push_back("close");
        }
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
        Result<std::optional<bytes::Bytes>> result = scripted.read(timeout, cancellation);
        ++read_count;
        if (result.has_value() && cancellation_to_trigger != nullptr && cancel_after_read_count.has_value() &&
            read_count == *cancel_after_read_count)
        {
            cancellation_to_trigger->cancel();
        }
        return result;
    }

    ScriptedCanFlashTransport scripted;
    Status reset_result;
    std::vector<std::string> lifecycle;
    std::vector<bytes::Bytes> writes;
    std::vector<std::chrono::milliseconds> read_timeouts;
    ToggleCancellation *cancellation_to_trigger = nullptr;
    ToggleCancellation *cancellation_on_open = nullptr;
    bytes::Bytes cancel_prefix;
    std::optional<std::size_t> cancel_after_read_count;
    std::size_t read_count{};
    std::vector<std::string> *timeline = nullptr;
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

void configure_and_open(SubaruDensoSh7058CanDieselExecutor& executor, const FlashPlan& plan,
                        ICanFlashTransport& transport)
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
    transport.expectWrite(request(bytes::Bytes{0xAA}));
    transport.queueRead(response(bytes::Bytes{0xEA, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x9A}));
    transport.expectWrite(request(bytes::Bytes{0x09, 0x02}));
    transport.queueRead(response(bytes::Bytes{0x49, 0x02, 0x00, 0x56, 0x49, 0x4E}));
    transport.expectWrite(request(bytes::Bytes{0x09, 0x04}));
    bytes::Bytes cal = response(bytes::Bytes{0x49, 0x04, 0x00});
    cal.insert(cal.end(), cal_id.begin(), cal_id.end());
    transport.queueRead(cal);
    transport.expectWrite(request(bytes::Bytes{0x09, 0x06}));
    transport.queueRead(response(bytes::Bytes{0x49, 0x06, 0x00, 0x12, 0x34}));
}

void script_tolerated_identity_failures(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request(bytes::Bytes{0xAA}));
    transport.queue_no_frame();
    transport.expectWrite(request(bytes::Bytes{0x09, 0x02}));
    transport.queueRead(response(bytes::Bytes{0x7F, 0x09, 0x12}));
    transport.expectWrite(request(bytes::Bytes{0x09, 0x04}));
    transport.queueRead(response(bytes::Bytes{0x49}));
    transport.expectWrite(request(bytes::Bytes{0x09, 0x06}));
    transport.queue_error(ErrorKind::Timeout, "tolerated CVN timeout");
}

void script_session_selection(ScriptedCanFlashTransport& transport, bool fallback_to_43 = false)
{
    transport.expectWrite(request(bytes::Bytes{0x10, 0x03}));
    if (fallback_to_43)
    {
        transport.queueRead(response(bytes::Bytes{0x7F, 0x10, 0x12}));
    }
    else
    {
        transport.queueRead(response(bytes::Bytes{0x50, 0x03}));
    }
    transport.expectWrite(request(bytes::Bytes{0x10, 0x43}));
    if (fallback_to_43)
    {
        transport.queueRead(response(bytes::Bytes{0x50, 0x43}));
    }
    else
    {
        transport.queueRead(response(bytes::Bytes{0x7F, 0x10, 0x12}));
    }
}

void script_stock_security_and_programming(ScriptedCanFlashTransport& transport, bool fallback_to_43 = false,
                                           bool accept_key = true)
{
    transport.expectWrite(request(bytes::Bytes{0x27, 0x01}));
    transport.queueRead(response(bytes::Bytes{0x67, 0x01, 0x11, 0x22, 0x33, 0x44}));
    transport.expectWrite(request(bytes::Bytes{0x27, 0x02, 0x35, 0xB6, 0x83, 0xBF}));
    if (!accept_key)
    {
        transport.queueRead(response(bytes::Bytes{0x7F, 0x27, 0x35}));
        return;
    }
    transport.queueRead(response(bytes::Bytes{0x67, 0x02}));
    transport.expectWrite(request(fallback_to_43 ? bytes::Bytes{0x10, 0x42} : bytes::Bytes{0x10, 0x02}));
    transport.queueRead(response(fallback_to_43 ? bytes::Bytes{0x50, 0x42} : bytes::Bytes{0x50, 0x02}));
}

void script_bootloader_connection(ScriptedCanFlashTransport& transport, bool fallback_to_43 = false,
                                  bool accept_key = true, bool tolerate_identity = false)
{
    script_kernel_probe_timeout(transport);
    if (tolerate_identity)
    {
        script_tolerated_identity_failures(transport);
    }
    else
    {
        script_identity_queries(transport);
    }
    script_session_selection(transport, fallback_to_43);
    script_stock_security_and_programming(transport, fallback_to_43, accept_key);
}

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

void queue_upload_b6_reply(ScriptedCanFlashTransport& transport, UploadB6Reply reply)
{
    switch (reply)
    {
    case UploadB6Reply::NoFrame:
        transport.queue_no_frame();
        return;
    case UploadB6Reply::Timeout:
        transport.queue_error(ErrorKind::Timeout, "B6 timeout");
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
        transport.queue_error(ErrorKind::Internal, "stale B6 adapter error");
        return;
    case UploadB6Reply::Cancelled:
        transport.queue_error(ErrorKind::Cancelled, "B6 cancellation");
        return;
    case UploadB6Reply::Disconnected:
        transport.queue_error(ErrorKind::Disconnected, "B6 disconnect");
        return;
    }
}

void script_129_byte_kernel_upload(ScriptedCanFlashTransport& transport, std::uint32_t address,
                                   bytes::Bytes final_kernel_id = kernel_id_response(),
                                   UploadB6Reply b6_reply = UploadB6Reply::NoFrame,
                                   bytes::Bytes kernel_start_response = response(bytes::Bytes{0x71, 0x01, 0x02, 0x02,
                                                                                              0x02}),
                                   bool queue_post_upload_probe = true)
{
    bytes::Bytes download{0x34, 0x04, 0x33};
    const bytes::Bytes encoded_address = u24(address);
    download.insert(download.end(), encoded_address.begin(), encoded_address.end());
    const bytes::Bytes encoded_length = u24(0x100);
    download.insert(download.end(), encoded_length.begin(), encoded_length.end());
    transport.expectWrite(request(download));
    transport.queueRead(response(bytes::Bytes{0x74, 0x20}));

    bytes::Bytes first{0xB6};
    const bytes::Bytes first_address = u24(address);
    first.insert(first.end(), first_address.begin(), first_address.end());
    for (int word = 0; word < 32; ++word)
    {
        first.insert(first.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    transport.expectWrite(request(first));
    queue_upload_b6_reply(transport, b6_reply);

    bytes::Bytes second{0xB6};
    const bytes::Bytes second_address = u24(address + 0x80);
    second.insert(second.end(), second_address.begin(), second_address.end());
    second.insert(second.end(), {0xC0, 0x41, 0xD4, 0xCA});
    for (int word = 0; word < 30; ++word)
    {
        second.insert(second.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    second.insert(second.end(), {0x42, 0x61, 0xDB, 0x2C});
    transport.expectWrite(request(second));
    queue_upload_b6_reply(transport, b6_reply);

    bytes::Bytes final{0xB6};
    const bytes::Bytes final_address = u24(address + 0x100);
    final.insert(final.end(), final_address.begin(), final_address.end());
    transport.expectWrite(request(final));
    queue_upload_b6_reply(transport, b6_reply);

    transport.expectWrite(request(bytes::Bytes{0x37}));
    transport.queueRead(response(bytes::Bytes{0x77}));
    transport.expectWrite(request(bytes::Bytes{0x31, 0x01, 0x02, 0x02, 0x02}));
    transport.queueRead(std::move(kernel_start_response));
    if (queue_post_upload_probe)
    {
        transport.expectWrite(kernel_id_request());
        transport.queueRead(final_kernel_id);
    }
}

void script_zero_read_pages(ScriptedCanFlashTransport& transport, std::uint32_t rom_size)
{
    constexpr std::uint32_t kPageSize = 0x400;
    constexpr auto first_page_prefix = std::to_array<bytes::Byte>(
        {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x0F, 0xED, 0xCB, 0xA9, 0x87, 0x65, 0x43, 0x21});
    constexpr auto last_page_prefix = std::to_array<bytes::Byte>(
        {0xA5, 0x5A, 0xC3, 0x3C, 0x69, 0x96, 0xF0, 0x0D, 0xD0, 0x0F, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67});
    for (std::uint32_t address = 0; address < rom_size; address += kPageSize)
    {
        bytes::Bytes payload{0x00,
                             static_cast<bytes::Byte>(address >> 16U),
                             static_cast<bytes::Byte>(address >> 8U),
                             static_cast<bytes::Byte>(address),
                             0x04,
                             0x00};
        transport.expectWrite(beef_request(0x03, payload));
        bytes::Bytes page(kPageSize, bytes::Byte{0});
        if (address == 0)
        {
            std::copy(first_page_prefix.begin(), first_page_prefix.end(), page.begin());
        }
        if (address + kPageSize == rom_size)
        {
            std::copy(last_page_prefix.begin(), last_page_prefix.end(), page.begin());
        }
        transport.queueRead(beef_response(0x43, page));
    }
}

void script_crc(ScriptedCanFlashTransport& transport, const BlockFixture& block, std::uint32_t crc,
                std::optional<Result<std::optional<bytes::Bytes>>> stale = std::nullopt)
{
    bytes::Bytes payload = be32(block.start);
    payload.push_back(0x00);
    const bytes::Bytes length = u24(block.length);
    payload.insert(payload.end(), length.begin(), length.end());
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

void script_compare(ScriptedCanFlashTransport& transport, const std::array<BlockFixture, 16>& blocks,
                    std::optional<std::size_t> mismatch, bool a5_block_matches = false)
{
    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
        std::uint32_t crc = blocks[index].zero_crc;
        if (index == 8 && a5_block_matches)
        {
            // Independently fixed CRC-32 for 0x18000 bytes of 0xA5.
            crc = 0xAAA0B108;
        }
        if (mismatch.has_value() && *mismatch == index)
        {
            crc ^= 1U;
        }
        script_crc(transport, blocks[index], crc);
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
    bytes::Bytes chunk(0x200, fill);
    for (std::uint32_t address = block.start; address < block.start + block.length; address += 0x200)
    {
        bytes::Bytes payload = be32(address);
        payload.insert(payload.end(), chunk.begin(), chunk.end());
        transport.expectWrite(beef_request(0x22, payload));
        transport.queueRead(beef_response(0x62));
        if ((address + 0x200 - block.start) % 0x1000 == 0)
        {
            const std::uint32_t commit_address = address + 0x200 - 0x1000;
            bytes::Bytes commit = be32(commit_address);
            const bytes::Bytes size = be16(0x1000);
            commit.insert(commit.end(), size.begin(), size.end());
            const std::uint32_t crc = fill == 0xA5 ? 0x958BA140U : 0xF722EF49U;
            const bytes::Bytes crc_bytes = be32(crc);
            commit.insert(commit.end(), crc_bytes.begin(), crc_bytes.end());
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

bool has_exact_log(const RecordingEventSink& events, LogLevel level, std::string_view message)
{
    return std::any_of(events.logs.begin(), events.logs.end(), [level, message](const auto& record)
                       { return record.first == level && record.second == message; });
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

std::vector<RecordedPhaseProgress> phase_records(const RecordingEventSink& events, std::string_view phase)
{
    std::vector<RecordedPhaseProgress> selected;
    std::copy_if(events.phase_progress_calls.begin(), events.phase_progress_calls.end(), std::back_inserter(selected),
                 [phase](const RecordedPhaseProgress& record) { return record.phase_name == phase; });
    return selected;
}

TEST(SubaruDensoSh7058CanDieselExecutor, TransportSetupUsesExactIsoConfigurationForBothGenerations)
{
    SubaruDensoSh7058CanDieselExecutor executor;
    for (const DieselVariant& variant : kVariants)
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

TEST(SubaruDensoSh7058CanDieselExecutor, BoundAttemptPreservesResetQuietPeriodConfigureOpenOrder)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto transport = std::make_unique<RecordingCanTransport>();
    RecordingCanTransport *observed_transport = transport.get();
    ToggleCancellation cancellation;
    observed_transport->cancellation_on_open = &cancellation;
    RecordingClock clock;
    std::vector<std::string> timeline;
    clock.timeline = &timeline;
    observed_transport->timeline = &timeline;
    RecordingEventSink events;

    auto attempt = bind_flash_attempt(std::move(*plan), std::make_unique<SubaruDensoSh7058CanDieselExecutor>(),
                                      std::move(transport));
    const auto result = attempt->run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_THAT(observed_transport->lifecycle, ElementsAre("reset_connection", "configure", "open", "close"));
    EXPECT_THAT(clock.sleeps, ElementsAre(500ms));
    EXPECT_THAT(timeline, ElementsAre("reset_connection", "sleep:500", "configure", "open", "close"));
}

TEST(SubaruDensoSh7058CanDieselExecutor, StartupCancellationAfterResetSkipsConfigureAndOpen)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    auto transport = std::make_unique<RecordingCanTransport>();
    RecordingCanTransport *observed_transport = transport.get();
    ToggleCancellation cancellation;
    RecordingClock clock;
    std::vector<std::string> timeline;
    clock.timeline = &timeline;
    observed_transport->timeline = &timeline;
    clock.cancel_during_sleep = &cancellation;
    RecordingEventSink events;

    auto attempt = bind_flash_attempt(std::move(*plan), std::make_unique<SubaruDensoSh7058CanDieselExecutor>(),
                                      std::move(transport));
    const auto result = attempt->run(clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_THAT(observed_transport->lifecycle, ElementsAre("reset_connection"));
    EXPECT_THAT(clock.sleeps, ElementsAre(500ms));
    EXPECT_THAT(timeline, ElementsAre("reset_connection", "sleep:500"));
}

TEST(SubaruDensoSh7058CanDieselExecutor, AlreadyRunningKernelReadsBothLiteralRomGeometries)
{
    SubaruDensoSh7058CanDieselExecutor executor;
    for (const DieselVariant& variant : kVariants)
    {
        SCOPED_TRACE(variant.protocol);
        auto plan = plan_for(variant, FlashOperation::Read);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        RecordingCanTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport.scripted);
        script_zero_read_pages(transport.scripted, variant.rom_size);
        NeverCancelled cancellation;
        RecordingClock clock;
        RecordingEventSink events;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << result.error().detail;
        ASSERT_TRUE(result->read_bytes.has_value());
        EXPECT_EQ(result->read_bytes->size(), variant.rom_size);
        EXPECT_EQ(bytes::Bytes(result->read_bytes->begin(), result->read_bytes->begin() + 16),
                  (bytes::Bytes{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x0F, 0xED, 0xCB, 0xA9, 0x87, 0x65,
                                0x43, 0x21}));
        EXPECT_EQ(
            bytes::Bytes(result->read_bytes->end() - kReadPageSize, result->read_bytes->end() - kReadPageSize + 16),
            (bytes::Bytes{0xA5, 0x5A, 0xC3, 0x3C, 0x69, 0x96, 0xF0, 0x0D, 0xD0, 0x0F, 0xBE, 0xEF, 0x01, 0x23, 0x45,
                          0x67}));
        EXPECT_TRUE(transport.scripted.scriptConsumed());
        ASSERT_EQ(transport.writes.size(), 1U + variant.rom_size / kReadPageSize);
        EXPECT_EQ(transport.writes[1], (bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00,
                                                     0x00, 0x00, 0x04, 0x00}));
        const bytes::Bytes expected_last =
            variant.mcu == "SH7058d"
                ? bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x0F, 0xFC, 0x00, 0x04, 0x00}
                : bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x07,
                               0x03, 0x00, 0x17, 0xFC, 0x00, 0x04, 0x00};
        EXPECT_EQ(transport.writes.back(), expected_last);
        ASSERT_EQ(transport.read_timeouts.size(), 1U + variant.rom_size / kReadPageSize);
        EXPECT_EQ(transport.read_timeouts.front(), 800ms);
        EXPECT_TRUE(std::all_of(transport.read_timeouts.begin() + 1, transport.read_timeouts.end(),
                                [](std::chrono::milliseconds timeout) { return timeout == 2000ms; }));
        EXPECT_THAT(clock.sleeps, ElementsAre(200ms));
        EXPECT_THAT(events.notices, ElementsAre("Reading ROM, please wait..."));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info,
                                  "Connecting to Subaru 07+ Diesel 32-bit CAN bootloader, please wait..."));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Reading ROM from Subaru 07+ Diesel 32-bit using CAN"));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Kernel ID: KID"));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info,
                                  "Kernel read addr: 0x00000000 length: 0x00000400, 1024000 B/s      2 s"));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info,
                                  std::format("Kernel read addr: 0x{:08X} length: 0x00000400, 1024000 B/s      1 s",
                                              variant.rom_size - 0x400)));
        ASSERT_FALSE(events.phase_progress_calls.empty());
        EXPECT_EQ(events.phase_progress_calls.front(), (RecordedPhaseProgress{"Kernel", 1, 2, 0, 1}));
        EXPECT_EQ(events.phase_progress_calls.back(),
                  (RecordedPhaseProgress{"Read", 2, 2, static_cast<int>(variant.rom_size),
                                         static_cast<int>(variant.rom_size)}));
    }
}

TEST(SubaruDensoSh7058CanDieselExecutor, UploadsLiteralKernelAtEachGenerationAddressBeforeEnteringRead)
{
    for (const DieselVariant& variant : kVariants)
    {
        SCOPED_TRACE(variant.protocol);
        bytes::Bytes kernel_data(129, bytes::Byte{0});
        kernel_data.back() = 0x01;
        auto plan = plan_for(variant, FlashOperation::Read, {}, std::move(kernel_data));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanDieselExecutor executor;
        RecordingCanTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport.scripted);
        script_129_byte_kernel_upload(transport.scripted, variant.kernel_address);
        ToggleCancellation cancellation;
        PhaseCancellingEventSink events(cancellation, "Kernel", 1);
        RecordingClock clock;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_TRUE(transport.scripted.scriptConsumed());
        ASSERT_EQ(transport.writes.size(), 17U);
        const bytes::Bytes expected_download =
            variant.mcu == "SH7058d"
                ? bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x34, 0x04, 0x33, 0xFF, 0x40, 0x00, 0x00, 0x01, 0x00}
                : bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x34, 0x04, 0x33, 0xFE, 0xE0, 0x00, 0x00, 0x01, 0x00};
        EXPECT_EQ(transport.writes[10], expected_download);
        const bytes::Bytes first_prefix = variant.mcu == "SH7058d"
                                              ? bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0xFF, 0x40, 0x00}
                                              : bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0xFE, 0xE0, 0x00};
        ASSERT_EQ(transport.writes[11].size(), 136U);
        EXPECT_TRUE(std::equal(first_prefix.begin(), first_prefix.end(), transport.writes[11].begin()));
        EXPECT_EQ(bytes::Bytes(transport.writes[11].begin() + 8, transport.writes[11].begin() + 16),
                  (bytes::Bytes{0xE7, 0xE2, 0x14, 0x30, 0xE7, 0xE2, 0x14, 0x30}));
        const bytes::Bytes second_prefix = variant.mcu == "SH7058d"
                                               ? bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0xFF, 0x40, 0x80}
                                               : bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0xFE, 0xE0, 0x80};
        ASSERT_EQ(transport.writes[12].size(), 136U);
        EXPECT_TRUE(std::equal(second_prefix.begin(), second_prefix.end(), transport.writes[12].begin()));
        EXPECT_EQ(bytes::Bytes(transport.writes[12].begin() + 8, transport.writes[12].begin() + 12),
                  (bytes::Bytes{0xC0, 0x41, 0xD4, 0xCA}));
        EXPECT_EQ(bytes::Bytes(transport.writes[12].end() - 4, transport.writes[12].end()),
                  (bytes::Bytes{0x42, 0x61, 0xDB, 0x2C}));
        EXPECT_EQ(transport.writes[13], variant.mcu == "SH7058d"
                                            ? (bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0xFF, 0x41, 0x00})
                                            : (bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xB6, 0xFE, 0xE1, 0x00}));
        EXPECT_EQ(transport.writes[14], (bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x37}));
        EXPECT_EQ(transport.writes[15], (bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x31, 0x01, 0x02, 0x02, 0x02}));
        EXPECT_EQ(transport.writes[16], kernel_id_request());
        EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 500ms), 3);
        EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 10ms), 3);
        EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 800ms), 2);
        EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 50ms), 12);
        EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 100ms), 1);
        EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 200ms), 2);
        EXPECT_THAT(events.notices, ElementsAre("Preparing, please wait...", "Reading ROM, please wait..."));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info,
                                  "Connecting to Subaru 07+ Diesel 32-bit CAN bootloader, please wait..."));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info,
                                  "Initializing Subaru 07+ Diesel 32-bit CAN kernel upload, please wait..."));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Debug, "Data bytes sent: 0x256"));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Kernel uploaded, starting..."));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Requesting kernel ID..."));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Kernel ID: KID"));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Reading ROM from Subaru 07+ Diesel 32-bit using CAN"));
    }
}

TEST(SubaruDensoSh7058CanDieselExecutor, ProbeFallbackUploadsExactKernelAddressAndKeepsPostProbeStrict)
{
    for (const DieselVariant& variant : kVariants)
    {
        SCOPED_TRACE(variant.protocol);
        bytes::Bytes kernel_data(129, bytes::Byte{0});
        kernel_data.back() = 0x01;
        auto plan = plan_for(variant, FlashOperation::Read, {}, std::move(kernel_data));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanDieselExecutor executor;
        RecordingCanTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport.scripted);
        script_129_byte_kernel_upload(transport.scripted, variant.kernel_address,
                                      bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE});
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scripted.scriptConsumed());
        EXPECT_THAT(transport.read_timeouts, Contains(500ms));
        EXPECT_TRUE(has_log(events, LogLevel::Info, "Requesting ECU ID"));
        EXPECT_TRUE(has_log(events, LogLevel::Info, "Sending seed key"));
    }
}

TEST(SubaruDensoSh7058CanDieselExecutor, InitialProbeToleratesShortMalformedNegativeAndWrongIdFrames)
{
    const std::array<bytes::Bytes, 4> probe_replies{{
        {0x00, 0x00, 0x07},
        {0x00, 0x00, 0x07, 0xE8, 0xDE, 0xAD, 0x00, 0x04, 0x41, 0x4B, 0x49, 0x44},
        {0x00, 0x00, 0x07, 0xE8, 0x7F, 0x01, 0x22},
        {0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x04, 0x41, 0x4B, 0x49, 0x44},
    }};
    for (const bytes::Bytes& probe_reply : probe_replies)
    {
        SCOPED_TRACE(bytes::toHex(probe_reply));
        auto plan = plan_for(kVariants.front(), FlashOperation::Read);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanDieselExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectWrite(kernel_id_request());
        transport.queueRead(probe_reply);
        transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xAA});
        transport.queue_error(ErrorKind::Disconnected, "stop after tolerant initial probe");
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_EQ(transport.writesConsumed(), 2U);
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "No response from kernel, initialising ECU..."));
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Initializing connection..."));
    }
}

TEST(SubaruDensoSh7058CanDieselExecutor, EveryB6ReplyIsRawAndIgnoredExceptCancellationOrDisconnect)
{
    const std::array<UploadB6Reply, 6> tolerated{UploadB6Reply::NoFrame,    UploadB6Reply::Timeout,
                                                 UploadB6Reply::Short,      UploadB6Reply::Malformed,
                                                 UploadB6Reply::WrongCanId, UploadB6Reply::AdapterError};
    for (const UploadB6Reply reply : tolerated)
    {
        SCOPED_TRACE(static_cast<int>(reply));
        bytes::Bytes kernel_data(129, bytes::Byte{0});
        kernel_data.back() = 1;
        auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanDieselExecutor executor;
        RecordingCanTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport.scripted);
        script_129_byte_kernel_upload(transport.scripted, kVariants.front().kernel_address,
                                      bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE}, reply);
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;
        const auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scripted.scriptConsumed());
    }

    for (const auto [reply, expected] : {std::pair{UploadB6Reply::Cancelled, ErrorKind::Cancelled},
                                         std::pair{UploadB6Reply::Disconnected, ErrorKind::Disconnected}})
    {
        bytes::Bytes kernel_data(129, bytes::Byte{0});
        kernel_data.back() = 1;
        auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanDieselExecutor executor;
        RecordingCanTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport.scripted);
        script_129_byte_kernel_upload(transport.scripted, kVariants.front().kernel_address,
                                      bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE}, reply);
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;
        const auto result = executor.execute(*plan, transport, clock, cancellation, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, expected);
    }
}

TEST(SubaruDensoSh7058CanDieselExecutor, KernelStartAcceptsServiceOnlyPositiveAndRejectsMalformedOrWrongService)
{
    struct StartCase
    {
        std::string_view name;
        bytes::Bytes response_pdu;
        ErrorKind expected;
    };
    const std::array<StartCase, 3> cases{{
        {"service-only", response(bytes::Bytes{0x71}), ErrorKind::Cancelled},
        {"malformed", response(bytes::Bytes{0x00}), ErrorKind::BadResponse},
        {"wrong-service", response(bytes::Bytes{0x70, 0x01}), ErrorKind::BadResponse},
    }};
    for (const StartCase& test_case : cases)
    {
        SCOPED_TRACE(test_case.name);
        bytes::Bytes kernel_data(129, bytes::Byte{0});
        kernel_data.back() = 1;
        auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanDieselExecutor executor;
        RecordingCanTransport transport;
        configure_and_open(executor, *plan, transport);
        script_bootloader_connection(transport.scripted);
        script_129_byte_kernel_upload(transport.scripted, kVariants.front().kernel_address, kernel_id_response(),
                                      UploadB6Reply::NoFrame, test_case.response_pdu,
                                      test_case.expected == ErrorKind::Cancelled);
        ToggleCancellation cancellation;
        RecordingEventSink events;
        if (test_case.expected == ErrorKind::Cancelled)
        {
            PhaseCancellingEventSink cancelling_events(cancellation, "Kernel", 1);
            FakeClock clock;
            const auto result = executor.execute(*plan, transport, clock, cancellation, cancelling_events);
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().kind, test_case.expected);
        }
        else
        {
            NeverCancelled never_cancelled;
            FakeClock clock;
            const auto result = executor.execute(*plan, transport, clock, never_cancelled, events);
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().kind, test_case.expected);
        }
        EXPECT_TRUE(transport.scripted.scriptConsumed());
    }
}

TEST(SubaruDensoSh7058CanDieselExecutor, IdentityQueriesAndSessionThreeToFortyThreeFallbackPrecedeStrictSecurity)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_bootloader_connection(transport.scripted, true, false, true);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    EXPECT_TRUE(has_log(events, LogLevel::Error, "No valid response from ECU"));
    EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Sending seed key"));
    EXPECT_NE(std::find(transport.writes.begin(), transport.writes.end(),
                        bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x02, 0x35, 0xB6, 0x83, 0xBF}),
              transport.writes.end());
}

// The frozen diesel oracle checks the complete serial frame before looking
// at the security PDU: lines 393-408 of revision 59f4e442 reject a frame of
// five bytes or fewer as "No valid response from ECU". This catches a
// regression that would let a stripped, service-only 0x67 frame reach the
// later seed-payload validation and report the wrong operator record.
TEST(SubaruDensoSh7058CanDieselExecutor, ShortSecuritySeedFrameUsesTheOracleNoValidResponseRecord)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_probe_timeout(transport);
    script_identity_queries(transport);
    script_session_selection(transport);
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x01});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x67});
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    ASSERT_FALSE(events.logs.empty());
    EXPECT_EQ(events.logs.back(), (std::pair{LogLevel::Error, std::string{"No valid response from ECU"}}));
}

TEST(SubaruDensoSh7058CanDieselExecutor, ShortSecurityKeyFrameUsesTheOracleNoValidResponseRecord)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_probe_timeout(transport);
    script_identity_queries(transport);
    script_session_selection(transport);
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x01});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x67, 0x01, 0x11, 0x22, 0x33, 0x44});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x02, 0x35, 0xB6, 0x83, 0xBF});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x67});
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    ASSERT_FALSE(events.logs.empty());
    EXPECT_EQ(events.logs.back(), (std::pair{LogLevel::Error, std::string{"No valid response from ECU"}}));
}

// RequestDownload uses the generic legacy error slice (received.mid(8)),
// unlike the security requests' mid(4). The literal seven-byte negative
// frame therefore has no byte at offset eight and must retain the oracle's
// "Not a valid answer" record rather than the NRC's "Invalid key" text.
TEST(SubaruDensoSh7058CanDieselExecutor, RequestDownloadNegativeUsesTheGenericOracleErrorSlice)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_bootloader_connection(transport);
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x34, 0x04, 0x33, 0xFF, 0x40, 0x00, 0x00, 0x00, 0x80});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x7F, 0x34, 0x35});
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    ASSERT_FALSE(events.logs.empty());
    EXPECT_EQ(events.logs.back(),
              (std::pair{LogLevel::Error, std::string{"Wrong response from ECU: Not a valid answer"}}));
}

// Security negative replies use the separate received.mid(4) oracle slice,
// so this same NRC remains human-readable for the stock seed request.
TEST(SubaruDensoSh7058CanDieselExecutor, SecurityNegativeUsesTheSecurityOracleErrorSlice)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_probe_timeout(transport);
    script_identity_queries(transport);
    script_session_selection(transport);
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x01});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x7F, 0x27, 0x35});
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    ASSERT_FALSE(events.logs.empty());
    EXPECT_EQ(events.logs.back(), (std::pair{LogLevel::Error, std::string{"Wrong response from ECU: Invalid key"}}));
}

TEST(SubaruDensoSh7058CanDieselExecutor, MalformedNegativeWrongIdTimeoutAndDisconnectAreTypedAtStrictSecurity)
{
    enum class ReplyKind
    {
        Frame,
        NoFrame,
        Error,
    };
    struct ErrorCase
    {
        std::string_view name;
        ReplyKind kind;
        bytes::Bytes frame;
        ErrorKind injected;
        ErrorKind expected;
        std::string_view operator_record;
    };
    const std::array<ErrorCase, 5> cases{{
        {"malformed",
         ReplyKind::Frame,
         {0x00, 0x00, 0x07, 0xE8, 0x67},
         ErrorKind::Internal,
         ErrorKind::BadResponse,
         "No valid response from ECU"},
        {"negative",
         ReplyKind::Frame,
         {0x00, 0x00, 0x07, 0xE8, 0x7F, 0x27, 0x35},
         ErrorKind::Internal,
         ErrorKind::BadResponse,
         "Wrong response from ECU: Invalid key"},
        {"wrong-id",
         ReplyKind::Frame,
         {0x00, 0x00, 0x07, 0xE9, 0x67, 0x01, 0x11, 0x22, 0x33, 0x44},
         ErrorKind::Internal,
         ErrorKind::BadResponse,
         "Wrong response from ECU: Not a valid answer"},
        {"timeout", ReplyKind::NoFrame, {}, ErrorKind::Internal, ErrorKind::Timeout, "No valid response from ECU"},
        {"disconnect", ReplyKind::Error, {}, ErrorKind::Disconnected, ErrorKind::Disconnected, ""},
    }};
    for (const ErrorCase& test_case : cases)
    {
        SCOPED_TRACE(test_case.name);
        auto plan = plan_for(kVariants.front(), FlashOperation::Read);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanDieselExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_probe_timeout(transport);
        script_identity_queries(transport);
        script_session_selection(transport);
        transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0x27, 0x01});
        switch (test_case.kind)
        {
        case ReplyKind::Frame:
            transport.queueRead(test_case.frame);
            break;
        case ReplyKind::NoFrame:
            transport.queue_no_frame();
            break;
        case ReplyKind::Error:
            transport.queue_error(test_case.injected, "injected strict-security transport failure");
            break;
        }
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, test_case.expected);
        if (!test_case.operator_record.empty())
        {
            EXPECT_TRUE(has_exact_log(events, LogLevel::Error, test_case.operator_record));
        }
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh7058CanDieselExecutor, StrictUdsPendingRetriesAreBoundedAndNeverResend)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_probe_timeout(transport.scripted);
    script_identity_queries(transport.scripted);
    script_session_selection(transport.scripted);
    const bytes::Bytes seed_request{0x00, 0x00, 0x07, 0xE0, 0x27, 0x01};
    transport.scripted.expectWrite(seed_request);
    for (int reply = 0; reply < 11; ++reply)
    {
        transport.scripted.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x7F, 0x27, 0x78});
    }
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    EXPECT_EQ(std::count(transport.writes.begin(), transport.writes.end(), seed_request), 1);
    ASSERT_GE(transport.read_timeouts.size(), 11U);
    EXPECT_EQ(transport.read_timeouts[transport.read_timeouts.size() - 11], 2000ms);
    EXPECT_TRUE(std::all_of(transport.read_timeouts.end() - 10, transport.read_timeouts.end(),
                            [](std::chrono::milliseconds timeout) { return timeout == 3000ms; }));
    EXPECT_EQ(std::count_if(events.logs.begin(), events.logs.end(),
                            [](const auto& record)
                            {
                                return record.first == LogLevel::Debug &&
                                       record.second == "ECU reported responsePending for SID 0x27; waiting";
                            }),
              11);
}

TEST(SubaruDensoSh7058CanDieselExecutor, CancellationStopsAtPendingRetryBoundaryWithoutResending)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_probe_timeout(transport.scripted);
    script_identity_queries(transport.scripted);
    script_session_selection(transport.scripted);
    const bytes::Bytes seed_request{0x00, 0x00, 0x07, 0xE0, 0x27, 0x01};
    transport.scripted.expectWrite(seed_request);
    transport.scripted.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x7F, 0x27, 0x78});
    transport.scripted.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x67, 0x01, 0x11, 0x22, 0x33, 0x44});
    ToggleCancellation cancellation;
    transport.cancellation_to_trigger = &cancellation;
    transport.cancel_after_read_count = 8;
    RecordingClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(std::count(transport.writes.begin(), transport.writes.end(), seed_request), 1);
    EXPECT_EQ(transport.read_timeouts.back(), 3000ms);
    EXPECT_EQ(transport.scripted.writesConsumed(), transport.writes.size());
}

TEST(SubaruDensoSh7058CanDieselExecutor, TestWriteDisablesFlashAndValidatesWithoutCommitForBothGenerations)
{
    for (const DieselVariant& variant : kVariants)
    {
        SCOPED_TRACE(variant.protocol);
        auto plan = plan_for(variant, FlashOperation::TestWrite);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanDieselExecutor executor;
        RecordingCanTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport.scripted);
        script_compare(transport.scripted, blocks_for(variant), 0U);
        script_flash_init(transport.scripted, true);
        script_flash_block(transport.scripted, blocks_for(variant)[0], 0x00, true);
        script_compare(transport.scripted, blocks_for(variant), 0U);
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << result.error().detail;
        EXPECT_EQ(result->operation, FlashOperation::TestWrite);
        EXPECT_TRUE(transport.scripted.scriptConsumed());
        EXPECT_TRUE(has_log(events, LogLevel::Info, "Test write mode on, no actual flash write is performed"));
        EXPECT_TRUE(has_log(events, LogLevel::Info, "*** Test write PASS, it's ok to perform actual write! ***"));
        EXPECT_EQ(std::count_if(transport.writes.begin(), transport.writes.end(),
                                [](const bytes::Bytes& wire) { return wire.size() > 8 && wire[8] == 0x24; }),
                  0);
        EXPECT_NE(
            std::find(transport.writes.begin(), transport.writes.end(),
                      beef_request(0x23, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0xF7, 0x22, 0xEF, 0x49})),
            transport.writes.end());
        EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Write flash buffer: 0x00000000 (0% - 512000 B/s, ~ 1 s)"));
        ASSERT_FALSE(events.phase_progress_calls.empty());
        EXPECT_EQ(events.phase_progress_calls.front(), (RecordedPhaseProgress{"Kernel", 1, 4, 0, 1}));
        EXPECT_EQ(events.phase_progress_calls.back(), (RecordedPhaseProgress{"Complete", 4, 4, 1, 1}));
    }
}

TEST(SubaruDensoSh7058CanDieselExecutor, ProprietaryReadRejectsMalformedWrongOpcodeWrongIdAndTimeoutReplies)
{
    enum class ReplyKind
    {
        Frame,
        NoFrame,
    };
    struct ErrorCase
    {
        std::string_view name;
        ReplyKind kind;
        bytes::Bytes reply;
        ErrorKind expected;
        std::string_view operator_record;
    };
    const std::array<ErrorCase, 4> cases{{
        {"malformed-length",
         ReplyKind::Frame,
         {0x00, 0x00, 0x07, 0xE8, 0xBE, 0xEF, 0x00, 0x05, 0x43, 0x00},
         ErrorKind::BadResponse,
         "Wrong response from ECU: Not a valid answer"},
        {"wrong-opcode", ReplyKind::Frame, beef_response(0x44, bytes::Bytes(kReadPageSize, bytes::Byte{0})),
         ErrorKind::BadResponse, "Wrong response from ECU: Not a valid answer"},
        {"wrong-id",
         ReplyKind::Frame,
         {0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x01, 0x43},
         ErrorKind::BadResponse,
         "Wrong response from ECU: Not a valid answer"},
        {"timeout", ReplyKind::NoFrame, {}, ErrorKind::Timeout, "No valid response from ECU"},
    }};
    for (const ErrorCase& test_case : cases)
    {
        SCOPED_TRACE(test_case.name);
        auto plan = plan_for(kVariants.front(), FlashOperation::Read);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanDieselExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        transport.expectWrite(
            bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00});
        if (test_case.kind == ReplyKind::Frame)
        {
            transport.queueRead(test_case.reply);
        }
        else
        {
            transport.queue_no_frame();
        }
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, test_case.expected);
        EXPECT_TRUE(has_exact_log(events, LogLevel::Error, test_case.operator_record));
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh7058CanDieselExecutor, CrcStaleDrainToleratesContentButPropagatesTerminalErrors)
{
    for (const std::optional<ErrorKind> terminal :
         {std::optional<ErrorKind>{}, std::optional<ErrorKind>{ErrorKind::Cancelled},
          std::optional<ErrorKind>{ErrorKind::Disconnected}})
    {
        SCOPED_TRACE(terminal.has_value() ? static_cast<int>(*terminal) : -1);
        auto plan = plan_for(kVariants.front(), FlashOperation::Write);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanDieselExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        if (!terminal.has_value())
        {
            script_crc(transport, kSh7058Blocks[0], kSh7058Blocks[0].zero_crc,
                       Result<std::optional<bytes::Bytes>>{std::optional<bytes::Bytes>{bytes::Bytes{0x12, 0x34}}});
            for (std::size_t index = 1; index < kSh7058Blocks.size(); ++index)
            {
                script_crc(transport, kSh7058Blocks[index], kSh7058Blocks[index].zero_crc);
            }
        }
        else
        {
            script_crc(transport, kSh7058Blocks[0], kSh7058Blocks[0].zero_crc,
                       Result<std::optional<bytes::Bytes>>{fail(*terminal, "stale drain terminal")});
        }
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

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

TEST(SubaruDensoSh7058CanDieselExecutor, UnchangedWriteSkipsFlashForBothGenerations)
{
    for (const DieselVariant& variant : kVariants)
    {
        auto plan = plan_for(variant, FlashOperation::Write);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruDensoSh7058CanDieselExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, blocks_for(variant), std::nullopt);
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        const auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << result.error().detail;
        EXPECT_EQ(result->operation, FlashOperation::Write);
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_TRUE(
            has_exact_log(events, LogLevel::Info,
                          "*** Compare results no difference between ROM and ECU data, no flashing needed! ***"));
        EXPECT_THAT(events.notices, ElementsAre("Writing ROM, please wait..."));
        EXPECT_THAT(
            events.phase_progress_calls,
            ElementsAre(RecordedPhaseProgress{"Kernel", 1, 4, 0, 1}, RecordedPhaseProgress{"Kernel", 1, 4, 1, 1},
                        RecordedPhaseProgress{"Compare", 2, 4, 0, 16}, RecordedPhaseProgress{"Compare", 2, 4, 1, 16},
                        RecordedPhaseProgress{"Compare", 2, 4, 2, 16}, RecordedPhaseProgress{"Compare", 2, 4, 3, 16},
                        RecordedPhaseProgress{"Compare", 2, 4, 4, 16}, RecordedPhaseProgress{"Compare", 2, 4, 5, 16},
                        RecordedPhaseProgress{"Compare", 2, 4, 6, 16}, RecordedPhaseProgress{"Compare", 2, 4, 7, 16},
                        RecordedPhaseProgress{"Compare", 2, 4, 8, 16}, RecordedPhaseProgress{"Compare", 2, 4, 9, 16},
                        RecordedPhaseProgress{"Compare", 2, 4, 10, 16}, RecordedPhaseProgress{"Compare", 2, 4, 11, 16},
                        RecordedPhaseProgress{"Compare", 2, 4, 12, 16}, RecordedPhaseProgress{"Compare", 2, 4, 13, 16},
                        RecordedPhaseProgress{"Compare", 2, 4, 14, 16}, RecordedPhaseProgress{"Compare", 2, 4, 15, 16},
                        RecordedPhaseProgress{"Compare", 2, 4, 16, 16}, RecordedPhaseProgress{"Write", 3, 4, 0, 0},
                        RecordedPhaseProgress{"Complete", 4, 4, 0, 1}, RecordedPhaseProgress{"Complete", 4, 4, 1, 1}));
    }
}

TEST(SubaruDensoSh7058CanDieselExecutor, RealWriteErasesProgramsCommitsAndVerifiesOneSmallBlock)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Write);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport.scripted);
    script_compare(transport.scripted, kSh7058Blocks, 0U);
    script_flash_init(transport.scripted, false);
    script_flash_block(transport.scripted, kSh7058Blocks[0], 0x00, false);
    script_compare(transport.scripted, kSh7058Blocks, std::nullopt);
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    EXPECT_THAT(events.notices, ElementsAre("Writing ROM, please wait..."));
    EXPECT_TRUE(
        has_exact_log(events, LogLevel::Info, "Connecting to Subaru 07+ Diesel 32-bit CAN bootloader, please wait..."));
    EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Writing ROM to Subaru 07+ Diesel 32-bit using CAN"));
    const bytes::Bytes commit{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x0B, 0x24, 0x00,
                              0x00, 0x00, 0x00, 0x10, 0x00, 0xF7, 0x22, 0xEF, 0x49};
    EXPECT_EQ(std::count(transport.writes.begin(), transport.writes.end(), commit), 1);
    EXPECT_THAT(logs_starting_with(events, "Write flash buffer:"),
                ElementsAre("Write flash buffer: 0x00000000 (0% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000200 (12% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000400 (25% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000600 (37% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000800 (50% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000A00 (62% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000C00 (75% - 512000 B/s, ~ 1 s)",
                            "Write flash buffer: 0x00000E00 (87% - 512000 B/s, ~ 1 s)"));
    EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Block 0 reflash complete."));
    EXPECT_FALSE(has_log(events, LogLevel::Error, "*** ERROR IN FLASH PROCESS ***"));
    const auto write_progress = phase_records(events, "Write");
    ASSERT_EQ(write_progress.size(), 10U);
    EXPECT_EQ(write_progress.front(), (RecordedPhaseProgress{"Write", 3, 4, 0, 0x1000}));
    EXPECT_EQ(write_progress[8], (RecordedPhaseProgress{"Write", 3, 4, 0x0FFF, 0x1000}));
    EXPECT_EQ(write_progress.back(), (RecordedPhaseProgress{"Write", 3, 4, 0x1000, 0x1000}));
    EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 5ms), 32);
    EXPECT_EQ(std::count(clock.sleeps.begin(), clock.sleeps.end(), 200ms), 1);
}

TEST(SubaruDensoSh7058CanDieselExecutor, NonzeroLargeBlockCommitsEveryWindowWithOrderedChangedByteProgress)
{
    bytes::Bytes image(kVariants.back().rom_size, bytes::Byte{0});
    std::fill(image.begin() + 0x8000, image.begin() + 0x20000, bytes::Byte{0xA5});
    auto plan = plan_for(kVariants.back(), FlashOperation::Write, std::move(image));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport.scripted);
    script_compare(transport.scripted, kSh7059Blocks, std::nullopt);
    script_flash_init(transport.scripted, false);
    script_flash_block(transport.scripted, kSh7059Blocks[8], 0xA5, false);
    script_compare(transport.scripted, kSh7059Blocks, std::nullopt, true);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    const std::array<std::uint32_t, 24> expected_commit_addresses{{
        0x00008000, 0x00009000, 0x0000A000, 0x0000B000, 0x0000C000, 0x0000D000, 0x0000E000, 0x0000F000,
        0x00010000, 0x00011000, 0x00012000, 0x00013000, 0x00014000, 0x00015000, 0x00016000, 0x00017000,
        0x00018000, 0x00019000, 0x0001A000, 0x0001B000, 0x0001C000, 0x0001D000, 0x0001E000, 0x0001F000,
    }};
    std::vector<bytes::Bytes> commits;
    for (const bytes::Bytes& wire : transport.writes)
    {
        if (wire.size() == 19 && wire[8] == 0x24)
        {
            commits.push_back(wire);
        }
    }
    ASSERT_EQ(commits.size(), expected_commit_addresses.size());
    for (std::size_t index = 0; index < commits.size(); ++index)
    {
        SCOPED_TRACE(index);
        EXPECT_EQ(bytes::readU32Be(commits[index], 9), expected_commit_addresses[index]);
        EXPECT_EQ(bytes::Bytes(commits[index].begin(), commits[index].begin() + 9),
                  (bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x0B, 0x24}));
        EXPECT_EQ(bytes::Bytes(commits[index].begin() + 13, commits[index].end()),
                  (bytes::Bytes{0x10, 0x00, 0x95, 0x8B, 0xA1, 0x40}));
    }
    const auto write_progress = phase_records(events, "Write");
    ASSERT_EQ(write_progress.size(), 194U);
    EXPECT_EQ(write_progress.front(), (RecordedPhaseProgress{"Write", 3, 4, 0, 0x18000}));
    for (std::size_t index = 1; index < 192; ++index)
    {
        EXPECT_EQ(write_progress[index].done, static_cast<int>(index * kWriteChunkSize));
        EXPECT_EQ(write_progress[index].total, 0x18000);
        EXPECT_LT(write_progress[index - 1].done, write_progress[index].done);
    }
    EXPECT_EQ(write_progress[192], (RecordedPhaseProgress{"Write", 3, 4, 0x17FFF, 0x18000}));
    EXPECT_EQ(write_progress[193], (RecordedPhaseProgress{"Write", 3, 4, 0x18000, 0x18000}));
    EXPECT_TRUE(has_exact_log(events, LogLevel::Info, "Block 8 reflash complete."));
}

TEST(SubaruDensoSh7058CanDieselExecutor, CancellationIsRejectedBeforeAnyTransportWrite)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    ScriptedCanFlashTransport transport;
    ToggleCancellation cancellation;
    cancellation.cancel();
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

TEST(SubaruDensoSh7058CanDieselExecutor, CancellationDuringKernelProbeStopsBeforeProbeRead)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    transport.scripted.expectWrite(kernel_id_request());
    ToggleCancellation cancellation;
    transport.cancellation_to_trigger = &cancellation;
    transport.cancel_prefix = kernel_id_request();
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.scripted.writesConsumed(), 1U);
    EXPECT_TRUE(transport.read_timeouts.empty());
}

TEST(SubaruDensoSh7058CanDieselExecutor, CancellationDuringKernelUploadStopsBeforeNextBlock)
{
    bytes::Bytes kernel_data(129, bytes::Byte{0});
    kernel_data.back() = 0x01;
    auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_bootloader_connection(transport.scripted);
    script_129_byte_kernel_upload(transport.scripted, kVariants.front().kernel_address);
    ToggleCancellation cancellation;
    transport.cancellation_to_trigger = &cancellation;
    transport.cancel_prefix = {0x00, 0x00, 0x07, 0xE0, 0xB6};
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(std::count_if(transport.writes.begin(), transport.writes.end(),
                            [](const bytes::Bytes& wire) { return wire.size() >= 5 && wire[4] == 0xB6; }),
              1);
}

TEST(SubaruDensoSh7058CanDieselExecutor, UploadedKernelReachesFirstReadPageBeforeCancellation)
{
    bytes::Bytes kernel_data(129, bytes::Byte{0});
    kernel_data.back() = 1;
    auto plan = plan_for(kVariants.front(), FlashOperation::Read, {}, std::move(kernel_data));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_bootloader_connection(transport.scripted);
    script_129_byte_kernel_upload(transport.scripted, kVariants.front().kernel_address);
    const bytes::Bytes first_read_payload{0x00, 0x00, 0x00, 0x00, 0x04, 0x00};
    transport.scripted.expectWrite(beef_request(0x03, first_read_payload));
    transport.scripted.queueRead(beef_response(0x43, bytes::Bytes(kReadPageSize, bytes::Byte{0xA5})));
    ToggleCancellation cancellation;
    transport.cancellation_to_trigger = &cancellation;
    transport.cancel_after_read_count = 18;
    RecordingClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    EXPECT_EQ(transport.writes.back(), beef_request(0x03, first_read_payload));
}

TEST(SubaruDensoSh7058CanDieselExecutor, CancellationAtReadPageBoundaryStopsBeforeSecondPage)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Read);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    transport.expectWrite(
        bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00});
    bytes::Bytes encrypted_page;
    encrypted_page.reserve(kReadPageSize);
    for (std::size_t word = 0; word < kReadPageSize / 4; ++word)
    {
        encrypted_page.insert(encrypted_page.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    transport.queueRead(beef_response(0x43, encrypted_page));
    ToggleCancellation cancellation;
    PhaseCancellingEventSink events(cancellation, "Read", 0x400);
    FakeClock clock;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh7058CanDieselExecutor, CancellationAtCrcBoundaryStopsBeforeSecondBlock)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Write);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_crc(transport, kSh7058Blocks[0], kSh7058Blocks[0].zero_crc);
    ToggleCancellation cancellation;
    PhaseCancellingEventSink events(cancellation, "Compare", 1);
    FakeClock clock;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoSh7058CanDieselExecutor, CancellationAfterEraseReportsKernelRecoveryWarning)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Write);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare(transport, kSh7058Blocks, 0U);
    script_flash_init(transport, false);
    transport.expectWrite(beef_request(0x04));
    transport.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x05, 0x25, 0x00, 0x00, 0x00, 0x00});
    transport.queueRead(beef_response(0x65));
    ToggleCancellation cancellation;
    EraseCancellingEventSink events(cancellation);
    FakeClock clock;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(has_exact_log(events, LogLevel::Error,
                              "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most "
                              "likely still running and receiving commands!"));
}

TEST(SubaruDensoSh7058CanDieselExecutor, CancellationDuringBufferWriteReportsKernelRecoveryWarning)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Write);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport.scripted);
    script_compare(transport.scripted, kSh7058Blocks, 0U);
    script_flash_init(transport.scripted, false);
    transport.scripted.expectWrite(beef_request(0x04));
    transport.scripted.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
    transport.scripted.expectWrite(beef_request(0x25, bytes::Bytes{0x00, 0x00, 0x00, 0x00}));
    transport.scripted.queueRead(beef_response(0x65));
    transport.scripted.expectWrite(beef_request(0x22, bytes::Bytes(516, bytes::Byte{0})));
    ToggleCancellation cancellation;
    transport.cancellation_to_trigger = &cancellation;
    transport.cancel_prefix = {0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x02, 0x05, 0x22};
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(has_exact_log(events, LogLevel::Error,
                              "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most "
                              "likely still running and receiving commands!"));
}

TEST(SubaruDensoSh7058CanDieselExecutor, CancellationBeforeCommitReportsKernelRecoveryWarning)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Write);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare(transport, kSh7058Blocks, 0U);
    script_flash_init(transport, false);
    transport.expectWrite(beef_request(0x04));
    transport.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
    transport.expectWrite(beef_request(0x25, bytes::Bytes{0x00, 0x00, 0x00, 0x00}));
    transport.queueRead(beef_response(0x65));
    for (std::uint32_t address = 0; address < kCommitSize; address += kWriteChunkSize)
    {
        bytes::Bytes payload = be32(address);
        payload.resize(4 + kWriteChunkSize, bytes::Byte{0});
        transport.expectWrite(beef_request(0x22, payload));
        transport.queueRead(beef_response(0x62));
    }
    ToggleCancellation cancellation;
    PhaseCancellingEventSink events(cancellation, "Write", 0x0FFF);
    FakeClock clock;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(has_exact_log(events, LogLevel::Error,
                              "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most "
                              "likely still running and receiving commands!"));
}

TEST(SubaruDensoSh7058CanDieselExecutor, CancellationDuringCommitReplyReportsKernelRecoveryWarning)
{
    auto plan = plan_for(kVariants.front(), FlashOperation::Write);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruDensoSh7058CanDieselExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport.scripted);
    script_compare(transport.scripted, kSh7058Blocks, 0U);
    script_flash_init(transport.scripted, false);
    script_flash_block(transport.scripted, kSh7058Blocks[0], 0x00, false);
    ToggleCancellation cancellation;
    transport.cancellation_to_trigger = &cancellation;
    transport.cancel_prefix = {0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x0B, 0x24};
    FakeClock clock;
    RecordingEventSink events;

    const auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(std::count_if(transport.writes.begin(), transport.writes.end(),
                            [](const bytes::Bytes& wire) { return wire.size() == 19 && wire[8] == 0x24; }),
              1);
    EXPECT_TRUE(has_exact_log(events, LogLevel::Error,
                              "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most "
                              "likely still running and receiving commands!"));
}

} // namespace
} // namespace fastecu::flash
