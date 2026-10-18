#include "src/backend/flash/ecu/testing/mitsu_colt_m32r_can_scripts.h"

#include <algorithm>
#include <utility>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

namespace fastecu::flash::colt_testing
{

std::string_view McuFor(std::string_view protocol)
{
    return protocol == kProtocol512 ? kMcu512 : kMcu384;
}

bytes::Bytes Request(bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::AppendU32Be(out, 0x7e0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bytes::Bytes Response(bytes::ByteView tail)
{
    bytes::Bytes out;
    bytes::AppendU32Be(out, 0x7e8);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

bytes::Bytes Response(std::initializer_list<bytes::Byte> tail)
{
    return Response(bytes::ByteView{tail.begin(), tail.end()});
}

Result<FlashPlan> ReadPlan(std::string_view protocol)
{
    return BuildMitsuColtM32rCanPlan(FlashOperation::kRead, protocol, McuFor(protocol), std::nullopt);
}

Result<FlashPlan> WritePlan(bytes::Bytes rom, std::string_view protocol)
{
    return BuildMitsuColtM32rCanPlan(FlashOperation::kWrite, protocol, McuFor(protocol), std::move(rom));
}

bytes::Bytes WriteRom()
{
    bytes::Bytes rom(mitsu_colt_can::kTopRegionEnd, 0xA5);
    std::fill(rom.begin() + mitsu_colt_can::kUserspaceStart, rom.begin() + mitsu_colt_can::kTopRegionStart, 0x00);
    std::fill(rom.begin() + mitsu_colt_can::kTopRegionStart, rom.end(), 0xEE);
    std::fill_n(rom.begin() + mitsu_colt_can::kUserspaceStart, 18, 0xFF);
    rom[mitsu_colt_can::kUserspaceStart + 18] = 0x46;
    return rom;
}

bytes::Bytes WriteRom384()
{
    bytes::Bytes rom(0x60000, 0xA5);
    std::fill(rom.begin() + mitsu_colt_can::kUserspaceStart, rom.end(), 0x00);
    std::fill_n(rom.begin() + mitsu_colt_can::kUserspaceStart, 18, 0xFF);
    rom[mitsu_colt_can::kUserspaceStart + 18] = 0x46;
    return rom;
}

bytes::ByteView UserspaceOf(const bytes::Bytes& rom)
{
    return bytes::ByteView(rom.data() + mitsu_colt_can::kUserspaceStart,
                           mitsu_colt_can::kUserspaceEnd - mitsu_colt_can::kUserspaceStart);
}

bytes::ByteView TopRegionOf(const bytes::Bytes& rom)
{
    return bytes::ByteView(rom.data() + mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength);
}

bytes::Bytes AddressMarkedImage(std::uint32_t length)
{
    bytes::Bytes image;
    image.reserve(length);
    for (std::uint32_t addr = 0; addr < length; ++addr)
    {
        image.push_back(static_cast<bytes::Byte>(addr));
    }
    return image;
}

void ScriptFlashRead(ScriptedCanFlashTransport& transport, std::uint32_t start, std::uint32_t length, bytes::Byte fill)
{
    const auto section = transport.Section("flash read");
    for (std::uint32_t addr = start; addr < start + length; addr += mitsu_colt_can::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = start + length - addr;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < mitsu_colt_can::kFlashReadBlockSize ? remaining : mitsu_colt_can::kFlashReadBlockSize);
        bytes::Bytes reply = Response({0x63});
        reply.insert(reply.end(), chunk, fill);
        transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(addr, chunk)), reply);
    }
}

void ScriptFlashReadChunk(ScriptedCanFlashTransport& transport, std::uint32_t addr, bytes::Byte chunk_len,
                          bytes::Byte fill)
{
    const auto section = transport.Section("flash read chunk");
    bytes::Bytes reply = Response({0x63});
    reply.insert(reply.end(), chunk_len, fill);
    transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(addr, chunk_len)), reply);
}

void ScriptFlashReadData(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data)
{
    const auto section = transport.Section("flash read data");
    for (std::uint32_t offset = 0; offset < data.size(); offset += mitsu_colt_can::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = static_cast<std::uint32_t>(data.size()) - offset;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < mitsu_colt_can::kFlashReadBlockSize ? remaining : mitsu_colt_can::kFlashReadBlockSize);
        bytes::Bytes reply = Response({0x63});
        reply.insert(reply.end(), data.begin() + offset, data.begin() + offset + chunk);
        transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(start + offset, chunk)), reply);
    }
}

void ScriptAddressMarkedRead(ScriptedCanFlashTransport& transport, std::uint32_t length)
{
    const auto section = transport.Section("address marked read");
    const bytes::Bytes image = AddressMarkedImage(length);
    for (std::uint32_t addr = 0; addr < length; addr += mitsu_colt_can::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = length - addr;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < mitsu_colt_can::kFlashReadBlockSize ? remaining : mitsu_colt_can::kFlashReadBlockSize);
        transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(addr, chunk)));
        bytes::Bytes reply = Response({0x63});
        reply.insert(reply.end(), image.begin() + addr, image.begin() + addr + chunk);
        transport.QueueRead(reply);
    }
}

void ScriptBootloadHandshake(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("bootload handshake");
    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)),
                       Response({0x50, 0x85}));

    transport.Exchange(Request(mitsu_colt_can::BuildSecurityAccessSeedRequest()),
                       Response({0x67, 0x05, 0x11, 0x22, 0x33, 0x44}));

    transport.Exchange(Request(mitsu_colt_can::BuildSecurityAccessKey(mitsu_colt_can::SeedKey(kSeed))),
                       Response({0x67, 0x06}));
}

void ScriptUploadFrames(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data)
{
    const auto section = transport.Section("upload frames");
    transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(start, static_cast<std::uint32_t>(data.size()))),
                       Response({0x74}));

    for (const bytes::Bytes& chunk : mitsu_colt_can::BuildTransferDataFrames(data))
    {
        transport.Exchange(Request(chunk), Response({0x76}));
    }
}

void ScriptCrcCommit(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data,
                     std::optional<bytes::Bytes> crc_bytes)
{
    const auto section = transport.Section("crc commit");
    transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(mitsu_colt_can::kCrcTransferAddress,
                                                                    mitsu_colt_can::kCrcTransferSize)),
                       Response({0x74}));

    const std::uint16_t crc = mitsu_colt_can::Checksum(data);
    const bytes::Bytes crc_data = crc_bytes.value_or(bytes::ComposeBe(crc));
    transport.Exchange(Request(mitsu_colt_can::BuildTransferDataFrames(crc_data).front()), Response({0x76}));

    // [echo][status=0], the routine-id echo plus the CRC-match status byte
    // (colt_commented.S ~0x5aa0-0x5ad4); the executor now checks the latter.
    transport.Exchange(Request(mitsu_colt_can::BuildRoutineCheckCrc(start)), Response({0x71, 0xe1, 0x00}));
}

void ScriptUploadAndCommit(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data,
                           std::optional<bytes::Bytes> crc_bytes)
{
    const auto section = transport.Section("upload and commit");
    ScriptUploadFrames(transport, start, data);
    ScriptCrcCommit(transport, start, data, std::move(crc_bytes));
}

void ScriptUnlockAndErase(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("unlock and erase");
    transport.Exchange(Request(mitsu_colt_can::BuildRequestReflashUnlock()), Response({0x7b}));
    // [echo][status=0], the routine-id echo plus the erase-succeeded status
    // byte (colt_commented.S ~0x59c8-0x5a38); the executor now checks the
    // latter -- this is the exact wire shape a real ECU sends.
    transport.Exchange(Request(mitsu_colt_can::BuildRoutineErase()), Response({0x71, 0xe0, 0x00}));
}

void ScriptWrite384KiB(ScriptedCanFlashTransport& transport, const bytes::Bytes& rom)
{
    ScriptBootloadHandshake(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
    ScriptUnlockAndErase(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom), kUserspaceChecksumBytes);
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));
}

void ScriptWriteWithTopRegionBootstrap(ScriptedCanFlashTransport& transport, const bytes::Bytes& rom)
{
    ScriptBootloadHandshake(transport);
    // ECU reports 0xFF from the very first chunk: mismatch, so the
    // bootstrap runs. top_region_matches() stops at that first chunk rather
    // than reading the full 128KB before comparing.
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kFlashReadBlockSize, 0xFF);
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kEraseRedirectRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWriteRedirectRoutine);
    ScriptUnlockAndErase(transport);
    // The carrier address is kUserspaceStart, not kTopRegionStart: the redirect
    // routines add the +0x058000 offset themselves.
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    // Verify read-back returns what was written.
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    // Then the ordinary write proceeds.
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
    ScriptUnlockAndErase(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));
}

} // namespace fastecu::flash::colt_testing
