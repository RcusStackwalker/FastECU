#pragma once

#include "src/backend/dashboard/dashboard_document.h"

namespace fastecu::dashboard::test
{
inline DashboardDocument valid_document()
{
    return DashboardDocument{
        .metadata =
            DocumentMetadata{
                .format_version = 1,
                .name = "Colt Dashboard",
                .description = "Example",
            },
        .connection =
            CdbgConnectionProfile{
                .protocol = DashboardProtocol::Cdbg,
                .transport = DashboardTransport::RawCan,
                .bitrate = 500000,
                .identifier_width = CanIdentifierWidth::Standard,
                .request_id = 0x630,
                .reply_id = 0x631,
                .stream_instance = 0,
                .sampling_interval_ms = 50,
                .retry =
                    RetryPolicy{
                        .poll_timeout_ms = 100,
                        .silence_threshold = 3,
                        .reconnect_attempts = 3,
                        .reconnect_period_ms = 250,
                    },
                .preferred_adapter = std::nullopt,
            },
        .channels =
            {
                DashboardChannel{
                    .id = "CDBG_ENGINE_RPM",
                    .name = "Engine RPM",
                    .description = "engine_rpm uint16",
                    .address = 0x804cfc,
                    .length = 2,
                    .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
                    .conversions =
                        {
                            DashboardConversion{
                                .id = "conversion-1",
                                .expression = "x*1000/256",
                                .unit = "rpm",
                                .precision = 0,
                                .gauge_min = 0.0,
                                .gauge_max = 8000.0,
                                .gauge_step = 500.0,
                            },
                        },
                },
            },
        .cards = {},
    };
}
} // namespace fastecu::dashboard::test
