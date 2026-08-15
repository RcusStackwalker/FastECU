#pragma once

#include <array>
#include <cstdint>

namespace fastecu::flash
{

// Crypto constants shared by SubaruTcuCvtMitsuMh8111CanExecutor and
// SubaruTcuCvtMitsuMh8104CanExecutor.
//
// Task 6 (wave-3 close) compared both already-merged executors' seed/key
// derivation and payload encrypt/decrypt call shapes line by line against
// their legacy sources and confirmed these three tables are byte-identical
// between the two families. Everything else compared in that pass --
// connect_bootloader, erase, reflash_block's setup/chunk-loop/close, and
// checksum verify -- differs in real, deliberate ways (chunk size, retry
// counts, per-chunk receive timeout, and content-check strictness) and is
// NOT extracted here; folding those into a shared "policy-parameterized"
// helper would recreate the configurable state machine the tail design
// forbids. kIndexTransformation likewise stays a per-file constant,
// matching every sibling executor's own copy in this wave rather than
// becoming a shared crypto-constants header entry.

// Seed-key table (legacy generate_seed_key: mh8111 lines 908-913, mh8104
// lines 903-907), confirmed byte-identical by direct comparison.
const std::array<std::uint16_t, 16>& tcuCvtMitsuSeedKeyTable();

// Write-payload encrypt table (legacy encrypt_payload: mh8111 lines
// 947-948, mh8104 lines 935-936), confirmed byte-identical.
const std::array<std::uint16_t, 4>& tcuCvtMitsuEncryptTable();

// Read-payload decrypt table (legacy decrypt_payload: mh8111 lines
// 965-966, mh8104 lines 953-954), confirmed byte-identical.
const std::array<std::uint16_t, 4>& tcuCvtMitsuDecryptTable();

} // namespace fastecu::flash
