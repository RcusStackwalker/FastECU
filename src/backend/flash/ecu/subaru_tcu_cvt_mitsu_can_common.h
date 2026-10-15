#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <array>
#include <cstdint>

namespace fastecu::flash
{

// Pure crypto operations and tables shared by SubaruTcuCvtMitsuMh8111CanExecutor
// and SubaruTcuCvtMitsuMh8104CanExecutor, using the canonical stock transformation.
// Connection, transport, addressing, checksum acceptance and programming flow
// remain family-owned. Legacy citations below establish table provenance only.

bytes::Bytes TcuCvtMitsuSeedKey(bytes::ByteView seed);
bytes::Bytes TcuCvtMitsuEncryptRom(bytes::ByteView image);
bytes::Bytes TcuCvtMitsuDecryptPage(bytes::ByteView page);

// Seed-key table (legacy generate_seed_key: mh8111 lines 908-913, mh8104
// lines 903-907), confirmed byte-identical by direct comparison.
const std::array<std::uint16_t, 16>& TcuCvtMitsuSeedKeyTable();

// Write-payload encrypt table (legacy encrypt_payload: mh8111 lines
// 947-948, mh8104 lines 935-936), confirmed byte-identical.
const std::array<std::uint16_t, 4>& TcuCvtMitsuEncryptTable();

// Read-payload decrypt table (legacy decrypt_payload: mh8111 lines
// 965-966, mh8104 lines 953-954), confirmed byte-identical.
const std::array<std::uint16_t, 4>& TcuCvtMitsuDecryptTable();

} // namespace fastecu::flash
