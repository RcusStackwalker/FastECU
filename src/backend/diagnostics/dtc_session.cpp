#include "src/backend/diagnostics/dtc_session.h"

#include <array>
#include <chrono>
#include <string>
#include <utility>

#include "src/algorithms/diagnostics/dtc_parser.h"
#include "src/algorithms/diagnostics/nrc_parser.h"

namespace fastecu::diagnostics
{
namespace
{
using namespace std::chrono_literals;

constexpr auto kShortRead = 200ms;
constexpr auto kCanInitRead = 2000ms;
constexpr auto kBeforeVehicleInfo = 500ms;
constexpr auto kBetweenRequests = 250ms;
constexpr std::uint8_t kFiveBaudAddress = 0x33;
constexpr std::uint8_t kFastInitWakeup = 0x81;
constexpr std::uint32_t kCanSource = 0x7E0;
constexpr std::uint32_t kCanDestination = 0x7E8;
constexpr std::uint8_t kLiveData = 0x01;
constexpr std::uint8_t kStoredDtcs = 0x03;
constexpr std::uint8_t kClearDtcs = 0x04;
constexpr std::uint8_t kPendingDtcs = 0x07;
constexpr std::uint8_t kVehicleInfo = 0x09;
constexpr std::array<std::uint8_t, 7> kSupportPages{0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0};

struct KlineIds
{
    std::uint8_t start_byte;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};

constexpr KlineIds ids_for(ObdProtocol protocol)
{
    return protocol == ObdProtocol::Iso9141 ? KlineIds{0x68, 0xF1, 0x6A} : KlineIds{0xC0, 0xF1, 0x33};
}

std::string as_text(const bytes::Bytes& data)
{
    return std::string(data.begin(), data.end());
}

class DtcRun
{
  public:
    DtcRun(const DtcRequest& request, IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
           IEventSink& events)
        : request_(request), link_(link), clock_(clock), cancellation_(cancellation), events_(events)
    {
    }

    Result<DtcReport> execute()
    {
        const Status outcome = body();
        // Today's select_operation epilogue; its results were never checked.
        static_cast<void>(link_.set_header(KlineHeader::None));
        static_cast<void>(link_.reset());
        if (!outcome.has_value())
        {
            return std::unexpected(outcome.error());
        }
        return std::move(report_);
    }

  private:
    Status body()
    {
        if (auto initialised = init(); !initialised.has_value())
        {
            return initialised;
        }
        if (auto info = vehicle_info(); !info.has_value())
        {
            return info;
        }
        return request_.operation == DtcOperation::Read ? read_dtcs() : clear_dtcs();
    }

    Status init()
    {
        switch (request_.protocol)
        {
        case ObdProtocol::Iso9141:
            return five_baud(ObdProtocol::Iso9141);
        case ObdProtocol::Iso14230:
            if (auto fast = fast_init(); fast.has_value() || fast.error().kind == ErrorKind::Cancelled)
            {
                return fast;
            }
            return five_baud(ObdProtocol::Iso14230);
        case ObdProtocol::Iso15765:
            return can_init();
        }
        return fail(ErrorKind::Internal, "unknown OBD protocol");
    }

    Status five_baud(ObdProtocol requested)
    {
        const std::string name(protocol_name(requested));
        const KlineIds ids = ids_for(requested);
        if (auto opened = link_.open(KlineLinkConfig{.header = KlineHeader::None,
                                                     .iso14230_connection = false,
                                                     .baud = 10400,
                                                     .start_byte = ids.start_byte,
                                                     .tester_id = ids.tester_id,
                                                     .target_id = ids.target_id});
            !opened.has_value())
        {
            return opened;
        }
        info("Testing " + name + " five baud init, please wait...");
        static_cast<void>(link_.set_p1_max(35ms)); // result never checked today
        auto response = link_.five_baud_init(kFiveBaudAddress);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        info("Init response: " + format_hex(*response));
        const bool j2534 = link_.uses_j2534();
        const std::optional<KlineHeader> header = five_baud_header(requested, *response, j2534);
        if (!j2534)
        {
            static_cast<void>(link_.set_p1_max(25ms));
        }
        if (!header.has_value())
        {
            error(name + " five baud init failed.");
            return fail(ErrorKind::BadResponse, name + " five baud init failed");
        }
        static_cast<void>(link_.set_header(*header));
        info(name + " five baud init succesfully completed.");
        return {};
    }

    Status fast_init()
    {
        const KlineIds ids = ids_for(ObdProtocol::Iso14230);
        if (auto opened = link_.open(KlineLinkConfig{.header = KlineHeader::Iso14230,
                                                     .iso14230_connection = true,
                                                     .baud = 10400,
                                                     .start_byte = ids.start_byte,
                                                     .tester_id = ids.tester_id,
                                                     .target_id = ids.target_id});
            !opened.has_value())
        {
            return opened;
        }
        info("Initialising iso14230 fast init K-Line communications, please wait...");
        if (auto woke = link_.fast_init(bytes::Bytes{kFastInitWakeup}); !woke.has_value())
        {
            return woke; // silent, as today; the caller falls back to five-baud
        }
        auto frame = read_frame(kShortRead);
        if (!frame.has_value())
        {
            return std::unexpected(frame.error());
        }
        if (!frame->has_value() || !fast_init_accepted(**frame))
        {
            error("iso14230 fast init mode failed.");
            return fail(ErrorKind::BadResponse, "iso14230 fast init mode failed");
        }
        info("iso14230 fast init mode succesfully completed.");
        return {};
    }

    Status can_init()
    {
        if (auto opened = link_.open(CanLinkConfig{.iso15765 = true,
                                                   .bitrate = 500000,
                                                   .extended_id = false,
                                                   .source_id = kCanSource,
                                                   .destination_id = kCanDestination});
            !opened.has_value())
        {
            return opened;
        }
        info("Initialising iso15765 CAN communications, please wait...");
        const bytes::Bytes probe = build_request(ObdProtocol::Iso15765, kCanSource, bytes::Bytes{kLiveData, 0x00});
        if (auto written = link_.write(probe); !written.has_value())
        {
            return std::unexpected(written.error());
        }
        auto frame = link_.read(kCanInitRead, cancellation_);
        if (!frame.has_value())
        {
            return std::unexpected(frame.error());
        }
        const bytes::Bytes f = frame->value_or(bytes::Bytes{});
        if (f.size() <= 4)
        {
            return fail(ErrorKind::BadResponse, "no iso15765 init response");
        }
        if (f[4] == 0x7F)
        {
            // Today's code describes the NRC from offset 3, not 4.
            error("Wrong response from ECU: " + nrc_description(bytes::ByteView(f).subspan(3)));
            return fail(ErrorKind::BadResponse, "iso15765 init rejected");
        }
        if (f[4] != 0x41)
        {
            error("Wrong response from ECU: " + format_hex(f));
            return fail(ErrorKind::BadResponse, "iso15765 init wrong response");
        }
        info("iso15765 init mode succesfully completed.");
        return {};
    }

    Status vehicle_info()
    {
        info("Requesting vehicle info, please wait...");
        if (auto slept = clock_.sleep(kBeforeVehicleInfo, cancellation_); !slept.has_value())
        {
            return slept;
        }
        for (std::size_t page = 0; page < kSupportPages.size(); ++page)
        {
            auto response = request(kLiveData, kSupportPages.at(page), false);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (!response->empty())
            {
                info(format_pid_page_label(page, *response));
                info("Supported PIDs: " + format_supported_pids(page, *response));
                report_.supported_pids.push_back(SupportedPidPage{page, *response});
            }
            if (auto slept = clock_.sleep(kBetweenRequests, cancellation_); !slept.has_value())
            {
                return slept;
            }
        }
        struct Item
        {
            std::uint8_t mode;
            std::uint8_t pid;
            const char *label;
            std::optional<bytes::Bytes> DtcReport::*field;
            bool also_text;
        };
        static constexpr std::array<Item, 7> kItems{{
            {kLiveData, 0x01, "Status since DTCs cleared: ", &DtcReport::monitor_status, false},
            {kVehicleInfo, 0x01, "VIN length: ", &DtcReport::vin_length, false},
            {kVehicleInfo, 0x02, "VIN: ", &DtcReport::vin, true},
            {kVehicleInfo, 0x03, "CAL ID length: ", &DtcReport::cal_id_length, false},
            {kVehicleInfo, 0x04, "CAL ID: ", &DtcReport::cal_id, true},
            {kVehicleInfo, 0x05, "CAL ID num length: ", &DtcReport::cvn_length, false},
            {kVehicleInfo, 0x06, "CAL ID num: ", &DtcReport::cvn, false},
        }};
        for (const Item& item : kItems)
        {
            auto response = request(item.mode, item.pid, false);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (!response->empty())
            {
                info(std::string(item.label) + format_hex(*response));
                if (item.also_text)
                {
                    info(std::string(item.label) + as_text(*response));
                }
                report_.*item.field = *response;
            }
            if (auto slept = clock_.sleep(kBetweenRequests, cancellation_); !slept.has_value())
            {
                return slept;
            }
        }
        return {};
    }

    Status read_dtcs()
    {
        struct List
        {
            std::uint8_t mode;
            const char *label;
            const char *missing;
            std::vector<std::uint16_t> DtcReport::*field;
        };
        static constexpr std::array<List, 2> kLists{{
            {kStoredDtcs, "Stored DTCs: ", "no stored DTC response", &DtcReport::stored},
            {kPendingDtcs, "Pending DTCs: ", "no pending DTC response", &DtcReport::pending},
        }};
        for (const List& list : kLists)
        {
            auto response = request(list.mode, std::nullopt, true);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (response->empty())
            {
                return fail(ErrorKind::BadResponse, list.missing);
            }
            info(std::string(list.label) + format_hex(*response));
            report_.*list.field = decode_dtcs(*response);
            for (const std::uint16_t code : report_.*list.field)
            {
                info("DTC: " + dtc_description(code));
            }
            if (auto slept = clock_.sleep(kBetweenRequests, cancellation_); !slept.has_value())
            {
                return slept;
            }
        }
        info("Diagnostic trouble codes succesfully read!");
        return {};
    }

    Status clear_dtcs()
    {
        if (auto read = read_dtcs(); !read.has_value())
        {
            return read;
        }
        const std::size_t index = response_index(request_.protocol);
        if (auto written = link_.write(build_request(request_.protocol, kCanSource, bytes::Bytes{kClearDtcs}));
            !written.has_value())
        {
            return std::unexpected(written.error());
        }
        bool cleared = false;
        while (true)
        {
            auto frame = read_frame(kShortRead);
            if (!frame.has_value())
            {
                return std::unexpected(frame.error());
            }
            if (!frame->has_value())
            {
                break;
            }
            const bytes::Bytes& f = **frame;
            if (f.size() <= index)
            {
                continue; // today's loop keeps reading past a short frame
            }
            if (f[index] == 0x7F)
            {
                error("Wrong response from ECU: " + nrc_description(bytes::ByteView(f).subspan(index)));
                break;
            }
            if (f[index] != (kClearDtcs | 0x40U))
            {
                error("Wrong response from ECU: " + format_hex(f));
                break;
            }
            cleared = true;
            break;
        }
        if (!cleared)
        {
            return fail(ErrorKind::BadResponse, "clear DTCs not acknowledged");
        }
        report_.cleared = true;
        info("Diagnostic trouble codes succesfully cleared!");
        return {};
    }

    // Writes one request and collects frames until an empty read. An NRC or
    // wrong response is logged and ends collection with what was gathered.
    Result<bytes::Bytes> request(std::uint8_t mode, std::optional<std::uint8_t> pid, bool dtc_list)
    {
        bytes::Bytes payload{mode};
        if (pid.has_value())
        {
            payload.push_back(*pid);
        }
        if (auto written = link_.write(build_request(request_.protocol, kCanSource, payload)); !written.has_value())
        {
            return std::unexpected(written.error());
        }
        bytes::Bytes response;
        while (true)
        {
            auto frame = read_frame(kShortRead);
            if (!frame.has_value())
            {
                return std::unexpected(frame.error());
            }
            if (!frame->has_value())
            {
                break;
            }
            const bytes::Bytes& f = **frame;
            const ResponseCheck check = check_response(request_.protocol, f, mode, pid);
            if (check == ResponseCheck::Nrc)
            {
                error("Wrong response from ECU: " +
                      nrc_description(bytes::ByteView(f).subspan(response_index(request_.protocol))));
                break;
            }
            if (check == ResponseCheck::WrongId)
            {
                error("Wrong response from ECU: " + format_hex(f));
                break;
            }
            const bytes::Bytes data = dtc_list ? unframe_dtc_list_response(request_.protocol, f)
                                               : unframe_data_response(request_.protocol, f);
            response.insert(response.end(), data.begin(), data.end());
        }
        return response;
    }

    Result<IDiagnosticLink::OptionalBytes> read_frame(std::chrono::milliseconds timeout)
    {
        return link_.uses_j2534() ? link_.read(timeout, cancellation_) : link_.read_obd(timeout, cancellation_);
    }

    void info(const std::string& message)
    {
        events_.log(LogLevel::Info, message);
    }
    void error(const std::string& message)
    {
        events_.log(LogLevel::Error, message);
    }

    DtcRequest request_;
    IDiagnosticLink& link_;
    IClock& clock_;
    const ICancellationToken& cancellation_;
    IEventSink& events_;
    DtcReport report_;
};

} // namespace

Result<DtcReport> run_dtc_session(const DtcRequest& request, IDiagnosticLink& link, IClock& clock,
                                  const ICancellationToken& cancellation, IEventSink& events)
{
    return DtcRun(request, link, clock, cancellation, events).execute();
}

} // namespace fastecu::diagnostics
