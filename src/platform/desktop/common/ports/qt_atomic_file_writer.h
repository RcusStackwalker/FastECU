#pragma once
#include "src/backend/ports/atomic_file_writer.h"

class QtAtomicFileWriter : public fastecu::IAtomicFileWriter
{
  public:
    fastecu::Status replace(std::string_view handle,
                            std::span<const std::uint8_t> data) override;
};
