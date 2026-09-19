#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

constexpr std::uint32_t kDiagnosticRequestId = 0x7E0;
// Legacy serial_read_timeout (operation.h:46): every connect_bootloader() and
// read_mem() read, and the per-block 0x34 window setup in reflash_block.
constexpr auto kResponseTimeout = 2000ms;
// Legacy receive_timeout (operation.h:45): the 0xB6 data-frame read only
// (operation.cpp:781).
constexpr auto kDataFrameTimeout = 500ms;
// Legacy serial_read_long_timeout (operation.h:50): the retried 0x37 close and
// 0x31 02 02 01 checksum reads (operation.cpp:845, 893).
constexpr auto kRetryTimeout = 800ms;
// Legacy erase_mem reads with a literal 200 ms (operation.cpp:957).
constexpr auto kEraseTimeout = 200ms;
constexpr auto kShortDelay = 50ms;
constexpr auto kJumpDelay = 200ms;
// Legacy read_mem's delay(1) after every dumped page (operation.cpp:553).
constexpr auto kPageDelay = 1ms;
// Legacy reflash_block's delay(200) before every 0xB6 read (operation.cpp:780)
// and its delay(100) between a closed block and its checksum (line 886).
constexpr auto kDataFrameDelay = 200ms;
constexpr auto kChecksumDelay = 100ms;
// Legacy erase_mem's delay(500) before the erase read (operation.cpp:953).
constexpr auto kEraseDelay = 500ms;
// Legacy reflash_block retries both the close and the checksum 20 times
// (operation.cpp:842, 890); exhausting either fails the block.
constexpr int kRetryAttempts = 20;
// Every read_mem response is framed by the four-byte CAN id plus its one-byte
// service id; legacy strips exactly those five before keeping the payload
// (operation.cpp:519, received.remove(0, 5)).
constexpr std::size_t kPageHeaderSize = 5;

// Directly compared against legacy generate_seed_key(): the 32-entry index
// transformation is byte-identical to SsmProtocol::kIndexTransformationStock.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{
    0xF2CA, 0x2417, 0x21DE, 0x8475, 0x39AB, 0xF767, 0x6204, 0x6BE0,
    0xBC63, 0x5988, 0x2845, 0x9846, 0xEB97, 0x99DE, 0xC7DB, 0xEFAE,
};

// Legacy decrypt_payload (operation.cpp:1021): encrypt_payload's four words in
// reverse order. The read path decrypts what the kernel dumps; the write path
// encrypts the image before the first frame goes out.
constexpr std::array<std::uint16_t, 4> kDecryptTable{0x1075, 0x9E51, 0x8BEF, 0x3B61};
// Legacy encrypt_payload (operation.cpp:999).
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x3B61, 0x8BEF, 0x9E51, 0x1075};

// Legacy write_mem's block_modified table (operation.cpp:633-634): 16 entries
// for an 11-block M32R_512KB, so entries 11-15 are never consulted. Blocks 0-2
// (0x0000, 0x4000, 0x6000) hold the bootloader and are deliberately left
// alone; only blocks 3-10 (0x8000 through 0x80000) are reflashed.
constexpr std::array<bool, 16> kBlockModified{
    false, false, false, true, true, true, true, true, true, true, true, false, false, false, false, false,
};

bytes::Bytes framed(bytes::ByteView payload, std::uint32_t request_id)
{
    bytes::Bytes result;
    bytes::appendU32Be(result, request_id);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

// read_timeout is explicit at every call site: the connect and dump paths all
// use the 2000 ms serial_read_timeout, while the write path mixes 200 ms
// (erase), 500 ms (data frames) and 800 ms (close/checksum retries).
Result<std::optional<bytes::Bytes>> exchange_optional(ICanFlashTransport& transport, IClock& clock,
                                                      const ICancellationToken& cancellation, bytes::ByteView payload,
                                                      std::uint32_t request_id,
                                                      std::chrono::milliseconds delay_before_read,
                                                      std::chrono::milliseconds read_timeout)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before write");
    }
    if (const Status written = transport.write(framed(payload, request_id), cancellation); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after write");
    }
    if (delay_before_read > 0ms)
    {
        if (const Status slept = clock.sleep(delay_before_read, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    Result<std::optional<bytes::Bytes>> response = transport.read(read_timeout, cancellation);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after read");
    }
    return std::move(*response);
}

Result<bytes::Bytes> exchange(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              bytes::ByteView payload, std::uint32_t request_id,
                              std::chrono::milliseconds delay_before_read, std::chrono::milliseconds read_timeout)
{
    Result<std::optional<bytes::Bytes>> response =
        exchange_optional(transport, clock, cancellation, payload, request_id, delay_before_read, read_timeout);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from TCU");
    }
    return std::move(**response);
}

Status expect_prefix(bytes::ByteView response, std::initializer_list<bytes::Byte> prefix)
{
    constexpr std::size_t kCanIdPrefixSize = 4;
    if (response.size() < kCanIdPrefixSize + prefix.size())
    {
        return fail(ErrorKind::BadResponse, "response is too short");
    }
    if (!std::equal(prefix.begin(), prefix.end(), response.begin() + kCanIdPrefixSize))
    {
        return fail(ErrorKind::BadResponse, "wrong response from TCU");
    }
    return {};
}

Result<bytes::Bytes> request_prefix(ICanFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation, bytes::ByteView request,
                                    std::uint32_t request_id, std::chrono::milliseconds delay_before_read,
                                    std::chrono::milliseconds read_timeout, std::initializer_list<bytes::Byte> expected)
{
    Result<bytes::Bytes> response =
        exchange(transport, clock, cancellation, request, request_id, delay_before_read, read_timeout);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (const Status matched = expect_prefix(*response, expected); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    return response;
}

Result<std::optional<bytes::Bytes>>
non_fatal_prefix(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                 IEventSink& events, bytes::ByteView request, std::uint32_t request_id,
                 std::chrono::milliseconds delay_before_read, std::chrono::milliseconds read_timeout,
                 std::initializer_list<bytes::Byte> expected, std::string_view label)
{
    Result<std::optional<bytes::Bytes>> response =
        exchange_optional(transport, clock, cancellation, request, request_id, delay_before_read, read_timeout);
    if (!response.has_value())
    {
        // A port error or cancellation is not an ECU content mismatch and
        // cannot safely be swallowed by a legacy-tolerance branch.
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        events.log(LogLevel::Error, std::format("No valid response from TCU for {}", label));
        return std::optional<bytes::Bytes>{};
    }
    if (const Status matched = expect_prefix(**response, expected); !matched.has_value())
    {
        events.log(LogLevel::Error, std::format("Wrong response from TCU for {}: {}", label, bytes::toHex(**response)));
        return std::optional<bytes::Bytes>{};
    }
    return std::move(**response);
}

bytes::Bytes seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable, SsmProtocol::kIndexTransformationStock);
}

Status connect_bootloader(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                          IEventSink& events, const SubaruTcuHitachiM32rCanPlan& plan)
{
    // Step 1: a matching response means the resident kernel is already live
    // and the remaining seven exchanges must not be sent. A missing or
    // non-matching frame means normal initialization should continue.
    Result<std::optional<bytes::Bytes>> alive = exchange_optional(
        transport, clock, cancellation, bytes::Bytes{0x31, 0x02, 0x02, 0x01}, plan.request_id, 0ms, kResponseTimeout);
    if (!alive.has_value())
    {
        return std::unexpected(alive.error());
    }
    if (alive->has_value())
    {
        if (const Status matched = expect_prefix(**alive, {0x71, 0x02, 0x02, 0x03}); matched.has_value())
        {
            events.log(LogLevel::Info, "Kernel already running");
            return {};
        }
        events.log(LogLevel::Error, std::format("Wrong response from TCU: {}", bytes::toHex(**alive)));
    }
    else
    {
        events.log(LogLevel::Error, "No valid response from TCU");
    }

    // Steps 2 and 3: identity mismatches are diagnostic only in legacy.
    Result<std::optional<bytes::Bytes>> tcu_id =
        non_fatal_prefix(transport, clock, cancellation, events, bytes::Bytes{0xAA}, kDiagnosticRequestId, kShortDelay,
                         kResponseTimeout, {0xEA}, "TCU ID");
    if (!tcu_id.has_value())
    {
        return std::unexpected(tcu_id.error());
    }
    if (tcu_id->has_value())
    {
        constexpr std::size_t kTcuIdOffset = 8;
        constexpr std::size_t kTcuIdSize = 5;
        const bytes::ByteView frame{**tcu_id};
        if (frame.size() < kTcuIdOffset + kTcuIdSize)
        {
            events.log(LogLevel::Error, "TCU ID response is too short");
        }
        else
        {
            std::string decoded;
            decoded.reserve(kTcuIdSize * 2);
            for (const bytes::Byte value : frame.subspan(kTcuIdOffset, kTcuIdSize))
            {
                decoded += std::format("{:02X}", value);
            }
            events.log(LogLevel::Info, std::format("TCU ID: {}", decoded));
        }
    }

    Result<std::optional<bytes::Bytes>> cal_id =
        non_fatal_prefix(transport, clock, cancellation, events, bytes::Bytes{0x09, 0x04}, kDiagnosticRequestId,
                         kShortDelay, kResponseTimeout, {0x49, 0x04}, "CAL ID");
    if (!cal_id.has_value())
    {
        return std::unexpected(cal_id.error());
    }
    if (cal_id->has_value())
    {
        constexpr std::size_t kCalIdOffset = 7;
        const bytes::ByteView frame{**cal_id};
        if (frame.size() < kCalIdOffset + 1)
        {
            events.log(LogLevel::Error, "CAL ID response is too short");
        }
        else
        {
            const std::string decoded(frame.begin() + static_cast<std::ptrdiff_t>(kCalIdOffset), frame.end());
            events.log(LogLevel::Info, std::format("CAL ID: {}", decoded));
        }
    }

    // Step 4: enter the extended diagnostic session; mismatch is fatal.
    if (Result<bytes::Bytes> session =
            request_prefix(transport, clock, cancellation, bytes::Bytes{0x10, 0x03}, kDiagnosticRequestId, kShortDelay,
                           kResponseTimeout, {0x50, 0x03});
        !session.has_value())
    {
        return std::unexpected(session.error());
    }

    // Step 5: request the four-byte seed. The prefix alone only guarantees
    // six framed bytes, so guard the indices 6..9 explicitly.
    Result<bytes::Bytes> seed_response =
        request_prefix(transport, clock, cancellation, bytes::Bytes{0x27, 0x01}, kDiagnosticRequestId, kShortDelay,
                       kResponseTimeout, {0x67, 0x01});
    if (!seed_response.has_value())
    {
        return std::unexpected(seed_response.error());
    }
    if (seed_response->size() < 10)
    {
        return fail(ErrorKind::BadResponse, "seed response is too short");
    }

    // Step 6: derive and send the key.
    bytes::Bytes key_request{0x27, 0x02};
    const bytes::Bytes key = seed_key(bytes::ByteView{*seed_response}.subspan(6, 4));
    key_request.insert(key_request.end(), key.begin(), key.end());
    if (Result<bytes::Bytes> key_response =
            request_prefix(transport, clock, cancellation, key_request, kDiagnosticRequestId, kShortDelay,
                           kResponseTimeout, {0x67, 0x02});
        !key_response.has_value())
    {
        return std::unexpected(key_response.error());
    }

    // Step 7: legacy logs a bad jump response and continues (its return is
    // commented out), but preserves the 200 ms delay before the read.
    Result<std::optional<bytes::Bytes>> jump =
        non_fatal_prefix(transport, clock, cancellation, events, bytes::Bytes{0x10, 0x02}, plan.request_id, kJumpDelay,
                         kResponseTimeout, {0x50, 0x02}, "kernel jump");
    if (!jump.has_value())
    {
        return std::unexpected(jump.error());
    }
    if (jump->has_value())
    {
        events.log(LogLevel::Info, std::format("kernel jump response: {}", bytes::toHex(**jump)));
    }

    // Step 8, deliberate divergence 3: build the full four-byte payload in
    // bounds. Legacy reused a six-byte frame and wrote positions 6 and 7
    // beyond its QByteArray, which does not extend under Qt 6.
    if (Result<bytes::Bytes> recheck =
            request_prefix(transport, clock, cancellation, bytes::Bytes{0x31, 0x02, 0x02, 0x01}, plan.request_id, 0ms,
                           kResponseTimeout, {0x71, 0x02, 0x02, 0x03});
        !recheck.has_value())
    {
        return std::unexpected(recheck.error());
    }
    return {};
}

// Legacy read_mem (operation.cpp:395-616).
Result<bytes::Bytes> read_rom(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              IEventSink& events, const SubaruTcuHitachiM32rCanPlan& plan, const MemoryRegion& region)
{
    // Deliberate divergence 2: legacy subtracted 0x00100000 from a start
    // address of 0 (line 408), underflowing uint32_t to 0xFFF00000, which is
    // not less than 0x8000 and so walked straight past its own floor clamp
    // (line 409). The live path therefore asked the kernel for 0x80000 bytes
    // from 0xFFF00000 and then prepended a further 0x8000 zeros -- a
    // 0x88000-byte image for a 0x80000 ROM, which cannot be a valid dump. The
    // port asks for the window the clamp evidently intended: region.length
    // (0x78000) bytes from region.start (0x8000).
    events.log(LogLevel::Info, "Setting dump start & length...");
    bytes::Bytes window_request{0x34, 0x04, 0x33};
    bytes::appendU24Be(window_request, region.start);
    bytes::appendU24Be(window_request, region.length);
    if (Result<bytes::Bytes> window = request_prefix(transport, clock, cancellation, window_request, plan.request_id,
                                                     0ms, kResponseTimeout, {0x74, 0x20, 0x01, 0x04});
        !window.has_value())
    {
        return std::unexpected(window.error());
    }

    events.log(LogLevel::Info, "Start reading ROM, please wait...");
    const std::size_t expected_page_frame = kPageHeaderSize + plan.page_size;
    bytes::Bytes dumped;
    dumped.reserve(region.length);
    for (std::uint32_t offset = 0; offset < region.length; offset += plan.page_size)
    {
        // Legacy's stopRequested() check at the top of the dump loop (line 480).
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled during ROM read");
        }
        const std::uint32_t address = region.start + offset;
        bytes::Bytes page_request{0xB7};
        bytes::appendU24Be(page_request, address);
        Result<bytes::Bytes> page = request_prefix(transport, clock, cancellation, page_request, plan.request_id, 0ms,
                                                   kResponseTimeout, {0xF7});
        if (!page.has_value())
        {
            return std::unexpected(page.error());
        }
        // Stricter than legacy, which appended whatever followed the five
        // header bytes and so handed back a silently short or long image when
        // a page came back the wrong size.
        if (page->size() != expected_page_frame)
        {
            return fail(ErrorKind::BadResponse, std::format("page read at 0x{:06X} returned {} bytes, expected {}",
                                                            address, page->size(), expected_page_frame));
        }
        dumped.insert(dumped.end(), page->begin() + static_cast<std::ptrdiff_t>(kPageHeaderSize), page->end());
        events.progress(static_cast<int>(offset + plan.page_size), static_cast<int>(region.length));
        // Legacy's delay(1) (line 553): after the page is in hand, before the
        // next request goes out.
        if (const Status slept = clock.sleep(kPageDelay, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }

    events.log(LogLevel::Info, "ROM read complete");
    events.log(LogLevel::Info, "Sending stop command...");
    // Legacy logs a bad or missing stop answer and carries on: both of its
    // `return STATUS_ERROR` lines are commented out (lines 590 and 597).
    Result<std::optional<bytes::Bytes>> stop =
        non_fatal_prefix(transport, clock, cancellation, events, bytes::Bytes{0x37}, plan.request_id, 0ms,
                         kResponseTimeout, {0x77}, "dump stop");
    if (!stop.has_value())
    {
        return std::unexpected(stop.error());
    }

    // Deliberate divergence 4: a sized zero buffer. Legacy filled a
    // default-constructed, empty QByteArray through padBytes[i] for i in
    // 0..0x7FFF (lines 605-609), which does not extend under Qt 6.
    bytes::Bytes image(region.start, bytes::Byte{0x00});
    const bytes::Bytes decrypted = SsmProtocol::calculatePayload(dumped, static_cast<std::uint32_t>(dumped.size()),
                                                                 kDecryptTable, SsmProtocol::kIndexTransformationStock);
    image.insert(image.end(), decrypted.begin(), decrypted.end());
    return image;
}

// Legacy erase_mem (operation.cpp:925-966).
Status erase_flash(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                   IEventSink& events, const SubaruTcuHitachiM32rCanPlan& plan)
{
    events.log(LogLevel::Info, "Erasing TCU ROM...");
    // Deliberate divergence 5, the most dangerous defect in the family: legacy
    // waited 500 ms, read with a 200 ms timeout for a multi-second erase, then
    // indexed received.at(4), at(5) and at(6) with no length guard, and its
    // `return STATUS_ERROR` was commented out (operation.cpp:963). A failed,
    // short or absent erase was logged and reflash proceeded onto
    // possibly-unerased flash. Here any of the three stops the operation
    // before a single 0x34 or 0xB6 frame is sent. The delay, the timeout and
    // the expected bytes are legacy's.
    if (Result<bytes::Bytes> erased =
            request_prefix(transport, clock, cancellation, bytes::Bytes{0x31, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF},
                           plan.request_id, kEraseDelay, kEraseTimeout, {0x31, 0x02, 0x01});
        !erased.has_value())
    {
        events.log(LogLevel::Error, "Erasing error! Do not panic, do not reset the TCU immediately. The kernel is "
                                    "most likely still running and receiving commands!");
        return std::unexpected(erased.error());
    }
    return {};
}

// Legacy reflash_block (operation.cpp:712-923).
Status reflash_block(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                     IEventSink& events, const SubaruTcuHitachiM32rCanPlan& plan, const flashblock& block,
                     bytes::ByteView encrypted, std::uint32_t& written, std::uint32_t total)
{
    const std::uint32_t frame_size = plan.write_frame_size;
    const std::uint32_t frames = block.len / frame_size;
    // Legacy end_addr - start_address: whole frames only (operation.cpp:730).
    const std::uint32_t data_len = frames * frame_size;
    // Defensive, and unreachable on the validated M32R_512KB geometry: the
    // plan validator pins both the MCU and the 0x80000 image size, so every
    // flashed block lies inside the encrypted image. Legacy indexed
    // newdata[i + blockaddr] with no such check (operation.cpp:775); this
    // makes a future geometry change fail cleanly instead of reading out of
    // bounds while talking to flash hardware.
    if (block.start > encrypted.size() || data_len > encrypted.size() - block.start)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("block at 0x{:08X} lies outside the 0x{:X}-byte image", block.start, encrypted.size()));
    }
    events.log(LogLevel::Info, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.len));

    events.log(LogLevel::Info, "Setting flash start & length...");
    bytes::Bytes window_request{0x34, 0x04, 0x33};
    bytes::appendU24Be(window_request, block.start);
    bytes::appendU24Be(window_request, data_len);
    if (Result<bytes::Bytes> window = request_prefix(transport, clock, cancellation, window_request, plan.request_id,
                                                     0ms, kResponseTimeout, {0x74});
        !window.has_value())
    {
        return std::unexpected(window.error());
    }

    for (std::uint32_t frame = 0; frame < frames; ++frame)
    {
        // Legacy's stopRequested() check at the top of the frame loop
        // (operation.cpp:756), which returned STATUS_SUCCESS and so reported a
        // cancelled block as reflashed.
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled during block write");
        }
        const std::uint32_t address = block.start + (frame * frame_size);
        bytes::Bytes data_request{0xB6};
        bytes::appendU24Be(data_request, address);
        const auto offset = static_cast<std::ptrdiff_t>(address);
        data_request.insert(data_request.end(), encrypted.begin() + offset,
                            encrypted.begin() + offset + static_cast<std::ptrdiff_t>(frame_size));
        if (Result<bytes::Bytes> written_frame =
                request_prefix(transport, clock, cancellation, data_request, plan.request_id, kDataFrameDelay,
                               kDataFrameTimeout, {0xF6});
            !written_frame.has_value())
        {
            return std::unexpected(written_frame.error());
        }
        written += frame_size;
        events.progress(static_cast<int>(written), static_cast<int>(total));
    }

    events.log(LogLevel::Info, "Closing out flashing of this block...");
    // Legacy retries the close up to 20 times; one non-0x77 or absent reply is
    // logged and retried (both `return STATUS_ERROR` lines are commented out,
    // operation.cpp:868), but exhausting the 20 attempts fails the block
    // before the checksum is ever asked for (operation.cpp:881-884).
    bool closed = false;
    for (int attempt = 0; attempt < kRetryAttempts && !closed; ++attempt)
    {
        Result<std::optional<bytes::Bytes>> close =
            non_fatal_prefix(transport, clock, cancellation, events, bytes::Bytes{0x37}, plan.request_id, 0ms,
                             kRetryTimeout, {0x77}, "block close");
        if (!close.has_value())
        {
            return std::unexpected(close.error());
        }
        closed = close->has_value();
    }
    if (!closed)
    {
        return fail(ErrorKind::BadResponse,
                    std::format("block at 0x{:08X} did not close after {} attempts", block.start, kRetryAttempts));
    }

    if (const Status slept = clock.sleep(kChecksumDelay, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }

    events.log(LogLevel::Info, "Verifying checksum...");
    // The 0x71 02 02 answer is the block's only success condition: legacy
    // returns STATUS_SUCCESS from inside this loop and STATUS_ERROR from below
    // it (operation.cpp:890-922). A short, wrong or absent answer is retried.
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt)
    {
        Result<std::optional<bytes::Bytes>> checksum =
            non_fatal_prefix(transport, clock, cancellation, events, bytes::Bytes{0x31, 0x02, 0x02, 0x01},
                             plan.request_id, 0ms, kRetryTimeout, {0x71, 0x02, 0x02}, "block checksum");
        if (!checksum.has_value())
        {
            return std::unexpected(checksum.error());
        }
        if (checksum->has_value())
        {
            events.log(LogLevel::Info, std::format("Block at 0x{:08X} reflash complete.", block.start));
            return {};
        }
    }
    return fail(ErrorKind::BadResponse,
                std::format("block at 0x{:08X} checksum failed after {} attempts", block.start, kRetryAttempts));
}

// Legacy write_mem (operation.cpp:624-702).
Status write_rom(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                 IEventSink& events, const SubaruTcuHitachiM32rCanPlan& plan, const FlashPlan& flash_plan)
{
    const flashdev_t *device = find_flash_device(flash_plan.mcu_name());
    if (device == nullptr || device->fblocks == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "Subaru TCU Hitachi M32R CAN flash geometry is invalid");
    }
    if (!flash_plan.image().has_value())
    {
        return fail(ErrorKind::InvalidConfig, "Subaru TCU Hitachi M32R CAN write plan carries no ROM image");
    }

    // Legacy encrypts the whole image before the first frame leaves
    // (operation.cpp:641) and then indexes it by absolute flash address
    // (operation.cpp:775, newdata[i + blockaddr] over fblocks[0].start == 0).
    const bytes::Bytes& image = *flash_plan.image();
    const bytes::Bytes encrypted = SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()),
                                                                 kEncryptTable, SsmProtocol::kIndexTransformationStock);
    if (encrypted.size() != image.size())
    {
        return fail(ErrorKind::Internal, "encrypted image size does not match the ROM image");
    }

    const unsigned blocks = std::min<unsigned>(device->numblocks, static_cast<unsigned>(kBlockModified.size()));
    std::uint32_t total = 0;
    for (unsigned blockno = 0; blockno < blocks; ++blockno)
    {
        if (kBlockModified.at(blockno))
        {
            total += device->fblocks[blockno].len;
        }
    }
    if (total == 0)
    {
        events.log(LogLevel::Info, "*** No blocks require flash! ***");
        return {};
    }

    events.log(LogLevel::Info, "--- erasing TCU flash memory ---");
    if (const Status erased = erase_flash(transport, clock, cancellation, events, plan); !erased.has_value())
    {
        return erased;
    }

    events.log(LogLevel::Info, "--- start writing ROM file to ECU flash memory ---");
    std::uint32_t written = 0;
    for (unsigned blockno = 0; blockno < blocks; ++blockno)
    {
        if (!kBlockModified.at(blockno))
        {
            continue;
        }
        if (const Status flashed = reflash_block(transport, clock, cancellation, events, plan, device->fblocks[blockno],
                                                 encrypted, written, total);
            !flashed.has_value())
        {
            events.log(LogLevel::Error, std::format("Block {} reflash failed.", blockno));
            return flashed;
        }
    }
    return {};
}

Result<FlashExecutionResult> execute_transfer(const FlashPlan& plan, ICanFlashTransport& transport, IClock& clock,
                                              const ICancellationToken& cancellation, IEventSink& events,
                                              const SubaruTcuHitachiM32rCanPlan& parameters)
{
    if (plan.operation() == FlashOperation::Read)
    {
        Result<bytes::Bytes> image =
            read_rom(transport, clock, cancellation, events, parameters, plan.transfer_region());
        if (!image.has_value())
        {
            return std::unexpected(image.error());
        }
        return FlashExecutionResult{
            .operation = FlashOperation::Read,
            .read_bytes = std::move(*image),
            .rom_id = std::nullopt,
        };
    }
    // Deliberate divergence 1: TestWrite never reaches here -- the plan
    // validator rejects it with ErrorKind::Unsupported, because legacy's
    // reflash_block took a test_write_arg and never read it, so "test write"
    // performed a real erase and a real flash write.
    if (const Status flashed = write_rom(transport, clock, cancellation, events, parameters, plan);
        !flashed.has_value())
    {
        return std::unexpected(flashed.error());
    }
    return FlashExecutionResult{
        .operation = FlashOperation::Write,
        .read_bytes = std::nullopt,
        .rom_id = std::nullopt,
    };
}

} // namespace

Result<Iso15765Config> SubaruTcuHitachiM32rCanExecutor::transport_setup(const FlashPlan& plan) const
{
    if (const Status family = check_family(plan, FlashFamily::SubaruTcuHitachiM32rCan); !family.has_value())
    {
        return std::unexpected(family.error());
    }
    if (const Status valid = validate_subaru_tcu_hitachi_m32r_can_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& parameters = std::get<SubaruTcuHitachiM32rCanPlan>(plan.family_plan());
    return iso15765_config_from(parameters);
}

Result<FlashExecutionResult> SubaruTcuHitachiM32rCanExecutor::execute(const FlashPlan& plan,
                                                                      ICanFlashTransport& transport, IClock& clock,
                                                                      const ICancellationToken& cancellation,
                                                                      IEventSink& events)
{
    if (const Status family = check_family(plan, FlashFamily::SubaruTcuHitachiM32rCan); !family.has_value())
    {
        return std::unexpected(family.error());
    }
    if (const Status valid = validate_subaru_tcu_hitachi_m32r_can_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }
    const auto& parameters = std::get<SubaruTcuHitachiM32rCanPlan>(plan.family_plan());
    if (const Status connected = connect_bootloader(transport, clock, cancellation, events, parameters);
        !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    return execute_transfer(plan, transport, clock, cancellation, events, parameters);
}

} // namespace fastecu::flash
