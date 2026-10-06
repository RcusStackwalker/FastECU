#pragma once

#include <array>
#include <string>

#include "src/backend/definition/definition_model.h"

namespace fastecu::definition
{

struct MetadataField
{
    const char *xml_name; // Fixed, null-terminated XML name literal.
    std::string RomMetadata::*member;
};

// Shared names only: callers retain their own text normalization and extras.
inline constexpr std::array<MetadataField, 9> kEditableMetadataFields{{
    {"make", &RomMetadata::make},
    {"market", &RomMetadata::market},
    {"model", &RomMetadata::model},
    {"submodel", &RomMetadata::submodel},
    {"transmission", &RomMetadata::transmission},
    {"year", &RomMetadata::year},
    {"flashmethod", &RomMetadata::flash_method},
    {"memmodel", &RomMetadata::memory_model},
    {"checksummodule", &RomMetadata::checksum_module},
}};

} // namespace fastecu::definition
