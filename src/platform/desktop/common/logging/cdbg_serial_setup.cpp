#include "src/platform/desktop/common/logging/cdbg_serial_setup.h"

#include <format>
#include <string_view>

namespace fastecu::desktop::logging
{

fastecu::Status configure_raw_can(const RawCanSetupProfile& profile, const RawCanSetupActions& actions)
{
    const auto run = [](std::string_view description, const auto& action, auto value) -> fastecu::Status
    {
        if (!action || !action(value))
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig, std::format("failed to {}", description));
        }
        return {};
    };

    if (const auto configured = run("disable ISO 14230 mode", actions.set_iso14230, false); !configured.has_value())
    {
        return configured;
    }
    if (const auto configured = run("disable ISO 14230 header", actions.set_iso14230_header, false);
        !configured.has_value())
    {
        return configured;
    }
    if (const auto configured = run("enable raw CAN mode", actions.set_raw_can, true); !configured.has_value())
    {
        return configured;
    }
    if (const auto configured = run("disable ISO 15765 mode", actions.set_iso15765, false); !configured.has_value())
    {
        return configured;
    }
    if (const auto configured = run("set CAN identifier width", actions.set_identifier_width, profile.identifier_width);
        !configured.has_value())
    {
        return configured;
    }
    if (const auto configured = run("set CAN bitrate", actions.set_bitrate, profile.bitrate); !configured.has_value())
    {
        return configured;
    }
    if (const auto configured = run("set CAN reply identifier", actions.set_reply_id, profile.reply_id);
        !configured.has_value())
    {
        return configured;
    }

    return {};
}

} // namespace fastecu::desktop::logging
