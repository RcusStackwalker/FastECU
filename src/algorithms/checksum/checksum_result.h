#ifndef CHECKSUM_RESULT_H
#define CHECKSUM_RESULT_H

#include "src/algorithms/protocol/bytes.h"

#include <string>

struct ChecksumResult
{
    enum class Status
    {
        Unchanged,
        Corrected,
        Disabled,
        InvalidSize,
        UnsupportedRom,
        ParseError
    };

    Status status = Status::Unchanged;
    bytes::Bytes romData;
    std::string message;

    bool changed() const
    {
        return status == Status::Corrected;
    }
    bool ok() const
    {
        return status == Status::Unchanged || status == Status::Corrected || status == Status::Disabled;
    }
};

#endif // CHECKSUM_RESULT_H
