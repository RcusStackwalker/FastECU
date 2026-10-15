#pragma once

#include "src/algorithms/protocol/bytes.h"

namespace fastecu::flash
{

// Pure crypto operations shared by SubaruTcuCvtMitsuMh8111CanExecutor
// and SubaruTcuCvtMitsuMh8104CanExecutor, using the canonical stock transformation.
// Connection, transport, addressing, checksum acceptance and programming flow
// remain family-owned.

bytes::Bytes TcuCvtMitsuSeedKey(bytes::ByteView seed);
bytes::Bytes TcuCvtMitsuEncryptRom(bytes::ByteView image);
bytes::Bytes TcuCvtMitsuDecryptPage(bytes::ByteView page);

} // namespace fastecu::flash
