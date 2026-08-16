#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

// Decodes a 16-bit DTC (top 2 bits select category: 0=P, 1=C, 2=B, 3=U) into
// a human-readable description. The top two bits only select which of the
// four caller-supplied tables to consult; the lookup key is the remaining
// 14 bits (dtc & 0x3fff), so caller-supplied tables must be keyed by the
// 14-bit code, not the full dtc value. Falls back to "<prefix><4-digit hex
// code> - Unknown error code" if the table has no entry for it.
std::string dtc_description(std::uint16_t dtc, const std::unordered_map<int, std::string>& pCodes,
                            const std::unordered_map<int, std::string>& cCodes,
                            const std::unordered_map<int, std::string>& bCodes,
                            const std::unordered_map<int, std::string>& uCodes);

// Same decode against the standard P/C/B/U tables in dtc_tables.h. Prefer
// this over the table-taking overload; that one exists so tests can exercise
// the category selection against synthetic tables.
std::string dtc_description(std::uint16_t dtc);
