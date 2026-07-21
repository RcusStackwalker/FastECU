#ifndef DTC_PARSER_H
#define DTC_PARSER_H

#include <cstdint>
#include <string>
#include <unordered_map>

// Decodes a 16-bit DTC (top 2 bits select category: 0=P, 1=C, 2=B, 3=U) into
// a human-readable description, looking the full dtc value up in the
// category's caller-supplied table. Falls back to "<prefix><4-digit hex
// code> - Unknown error code" if the table has no entry for it.
std::string dtc_description(std::uint16_t dtc,
                            const std::unordered_map<int, std::string>& pCodes,
                            const std::unordered_map<int, std::string>& cCodes,
                            const std::unordered_map<int, std::string>& bCodes,
                            const std::unordered_map<int, std::string>& uCodes);

#endif // DTC_PARSER_H
