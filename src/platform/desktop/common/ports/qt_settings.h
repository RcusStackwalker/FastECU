#pragma once
#include <memory>
#include "src/backend/ports/settings.h"

class QSettings;

// Backed by a QSettings instance owned by the adapter.
class QtSettings : public fastecu::ISettings
{
  public:
    QtSettings();
    ~QtSettings() override;
    std::optional<std::string> get(std::string_view key) const override;
    void set(std::string_view key, std::string_view value) override;

  private:
    std::unique_ptr<QSettings> settings_;
};
