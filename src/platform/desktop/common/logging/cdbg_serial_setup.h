#pragma once

#include <cstdint>
#include <functional>

#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/ports/result.h"

namespace fastecu::desktop::logging
{

struct RawCanSetupProfile
{
    std::uint32_t bitrate;
    dashboard::CanIdentifierWidth identifier_width;
    std::uint32_t reply_id;
};

struct RawCanSetupActions
{
    std::function<bool(bool)> set_iso14230;
    std::function<bool(bool)> set_iso14230_header;
    std::function<bool(bool)> set_raw_can;
    std::function<bool(bool)> set_iso15765;
    std::function<bool(dashboard::CanIdentifierWidth)> set_identifier_width;
    std::function<bool(std::uint32_t)> set_bitrate;
    std::function<bool(std::uint32_t)> set_reply_id;
};

fastecu::Status configure_raw_can(const RawCanSetupProfile& profile, const RawCanSetupActions& actions);

} // namespace fastecu::desktop::logging
