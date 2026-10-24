#pragma once

#include <string>
#include <unordered_map>

// Diagnostic lookup tables, formerly the FileActions::neg_rsp_codes /
// dtc_[PBCU]xxxx_codes QHash statics of the legacy error_codes.h.
//
// The DTC tables are keyed by the 14-bit code with the two category bits
// masked off -- the category selects which table to consult, so it is not
// part of the key. NrcCodes() is keyed by the raw NRC byte.
//
// Generated data lives in dtc_tables.cpp. It was machine-transcribed once from
// the legacy error_codes.h, which no longer exists -- there is no
// regeneration path, and no input one could run against. Add new entries by
// editing dtc_tables.cpp directly and carefully.
const std::unordered_map<int, std::string>& NrcCodes();
const std::unordered_map<int, std::string>& DtcPCodes();
const std::unordered_map<int, std::string>& DtcBCodes();
const std::unordered_map<int, std::string>& DtcCCodes();
const std::unordered_map<int, std::string>& DtcUCodes();
