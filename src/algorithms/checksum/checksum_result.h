#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <string>

struct ChecksumResult
{
    enum class Status
    {
        kUnchanged,
        kCorrected,
        kDisabled,
        kInvalidSize,
        kUnsupportedRom,
        kParseError
    };

    Status status = Status::kUnchanged;
    bytes::Bytes rom_data;
    std::string message;

    bool Changed() const
    {
        return status == Status::kCorrected;
    }
    bool Ok() const
    {
        return status == Status::kUnchanged || status == Status::kCorrected || status == Status::kDisabled;
    }
};
