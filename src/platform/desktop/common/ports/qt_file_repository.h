#pragma once
#include "src/backend/ports/file_repository.h"

// Resolves handles to filesystem paths via QFile.
class QtFileRepository : public fastecu::IFileRepository
{
  public:
    fastecu::Result<std::vector<std::uint8_t>> Read(std::string_view handle) override;
    fastecu::Status Write(std::string_view handle, std::span<const std::uint8_t>) override;
};
