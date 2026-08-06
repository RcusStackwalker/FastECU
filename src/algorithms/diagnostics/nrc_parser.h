#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <string>
#include <unordered_map>

// Decodes a UDS/KWP2000 negative response frame (0x7f <sid> <nrc>) into a
// human-readable description, looking the NRC byte up in the caller-supplied
// table. Returns "Not a valid answer" if the frame is too short or is not a
// negative response, and "Unknown error code" if the NRC byte has no entry
// in `codes`.
std::string nrc_description(bytes::ByteView nrc, const std::unordered_map<int, std::string>& codes);

// Same decode against the standard NRC table in dtc_tables.h. Prefer this
// over the table-taking overload; that one exists so tests can exercise the
// frame handling against a synthetic table.
std::string nrc_description(bytes::ByteView nrc);
