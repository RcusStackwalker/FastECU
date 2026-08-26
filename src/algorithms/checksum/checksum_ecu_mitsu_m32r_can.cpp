#include "checksum_ecu_mitsu_m32r_can.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"

namespace
{
constexpr std::uint32_t kTargetChecksum = 0x5AA55AA5;
constexpr std::size_t kCorrectionWords = 0x3FFCE;
constexpr std::size_t kCorrectionWordCount = 5;

// Adjusting this word is how the factory balances the sum: stock 47110032
// ships a non-erased value here, and EcuFlash's mitsucan module rewrites the
// same slot.
constexpr std::size_t kBalanceSlot = 0x3FFC0;

// One ROM byte encodes how much flash the ECU's checker walks; on 47110032 it
// reads 0xC2, matching the hard-coded 0x60000 wrap in flash_crc_check_block().
constexpr std::size_t kAreaCodeOffset = 0x3FFCB;

struct AreaCode
{
    std::uint8_t code;
    std::size_t end;
};

// EcuFlash's mitsucan module reads the same byte and switches on exactly these
// seven values.
constexpr std::array kAreaCodes{
    AreaCode{0x40, 0x040000}, AreaCode{0x50, 0x080000}, AreaCode{0x60, 0x100000}, AreaCode{0x80, 0x140000},
    AreaCode{0xC0, 0x050000}, AreaCode{0xC2, 0x060000}, AreaCode{0xC3, 0x0C0000},
};

std::optional<std::size_t> areaEndFor(std::uint8_t code)
{
    const auto match = std::ranges::find(kAreaCodes, code, &AreaCode::code);
    return match == kAreaCodes.end() ? std::nullopt : std::optional{match->end};
}

// flash5013e_u8. The ECU's first pass skips 0x50000-0x5A000 whenever this
// byte is clear, while its second pass re-sums 0x56000-0x5F0D0 in full. The
// two overlap, so a cleared flag both changes the total this module computes
// and leaves rom_crc_finalized_check2() unable to agree with what the first
// pass latched -- the ROM-checksum DTC then fires whatever the balance word
// holds. The address is specific to the 47110032 family this module is routed
// to; stock images of that family ship 0x01 here.
constexpr std::size_t kExclusionFlag = 0x5013E;

// Every fixed offset this module reads must be present: the six correction
// words, and the checker flag further up. An image shorter than that carries
// no Mitsubishi M32R checksum layout at all.
constexpr std::size_t kCorrectionWordsEnd = kCorrectionWords + 2 + (kCorrectionWordCount * 4);
constexpr std::size_t kLayoutEnd = std::max(kCorrectionWordsEnd, kExclusionFlag + 1);

ChecksumResult unchangedWith(ChecksumResult::Status status, bytes::ByteView romView, std::string message)
{
    return {.status = status, .romData = bytes::Bytes(romView.begin(), romView.end()), .message = std::move(message)};
}
} // namespace

ChecksumResult ChecksumEcuMitsuM32rCan::calculate_checksum_result(bytes::ByteView romView)
{
    if (romView.size() < kLayoutEnd)
    {
        return unchangedWith(ChecksumResult::Status::InvalidSize, romView,
                             "ROM is too small to carry the Mitsubishi M32R checksum layout");
    }

    const std::optional<std::size_t> areaEnd = areaEndFor(romView[kAreaCodeOffset]);
    if (!areaEnd.has_value())
    {
        return unchangedWith(ChecksumResult::Status::Disabled, romView,
                             "Unrecognised checksum area code; checksums disabled");
    }
    if (*areaEnd > romView.size())
    {
        return unchangedWith(ChecksumResult::Status::Disabled, romView,
                             "Checksum area extends past the end of the ROM; checksums disabled");
    }

    if (romView[kExclusionFlag] == 0)
    {
        return unchangedWith(ChecksumResult::Status::UnsupportedRom, romView,
                             "ROM disables the ECU's own first-pass flash sweep, so its checksum check cannot pass; "
                             "no correction applied");
    }

    std::uint32_t checksum = 0;
    for (std::size_t offset = 0; offset < *areaEnd; offset += 4)
    {
        checksum += bytes::readU32Be(romView, offset);
    }
    checksum -= bytes::readU16Be(romView, kCorrectionWords);
    for (std::size_t index = 0; index < kCorrectionWordCount; ++index)
    {
        checksum -= bytes::readU32Be(romView, kCorrectionWords + 2 + (index * 4));
    }
    checksum += 0xFFFF;
    checksum -= 5;

    bytes::Bytes romData(romView.begin(), romView.end());
    if (checksum == kTargetChecksum)
    {
        return {.status = ChecksumResult::Status::Unchanged, .romData = std::move(romData)};
    }
    fastecu::checksum::internal::rebalanceU32Be(romData, kBalanceSlot, checksum, kTargetChecksum);
    return {.status = ChecksumResult::Status::Corrected,
            .romData = std::move(romData),
            .message = "Mitsubishi M32R CAN ECU Checksum"};
}
