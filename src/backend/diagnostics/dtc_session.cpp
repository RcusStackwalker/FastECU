#include "src/backend/diagnostics/dtc_session.h"

#include <array>
#include <chrono>
#include <string>
#include <tuple>
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

constexpr KlineIds IdsFor(ObdProtocol protocol)
{
    return protocol == ObdProtocol::kIso9141 ? KlineIds{0x68, 0xF1, 0x6A} : KlineIds{0xC0, 0xF1, 0x33};
}

std::string AsText(const bytes::Bytes& data)
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

    Result<DtcReport> Execute()
    {
        const Status outcome = Body();
        // Today's select_operation epilogue; its results were never checked.
        std::ignore = link_.SetHeader(KlineHeader::kNone);
        std::ignore = link_.Reset();
        if (!outcome.has_value())
        {
            return std::unexpected(outcome.error());
        }
        return std::move(report_);
    }

  private:
    Status Body()
    {
        if (auto initialised = Init(); !initialised.has_value())
        {
            return initialised;
        }
        if (auto info = VehicleInfo(); !info.has_value())
        {
            return info;
        }
        return request_.operation == DtcOperation::kRead ? ReadDtcs() : ClearDtcs();
    }

    Status Init()
    {
        switch (request_.protocol)
        {
        case ObdProtocol::kIso9141:
            return FiveBaud(ObdProtocol::kIso9141);
        case ObdProtocol::kIso14230:
            if (auto fast = FastInit(); fast.has_value() || fast.error().kind == ErrorKind::kCancelled)
            {
                return fast;
            }
            return FiveBaud(ObdProtocol::kIso14230);
        case ObdProtocol::kIso15765:
            return CanInit();
        }
        return Fail(ErrorKind::kInternal, "unknown OBD protocol");
    }

    Status FiveBaud(ObdProtocol requested)
    {
        const std::string name(ProtocolName(requested));
        const KlineIds ids = IdsFor(requested);
        if (auto opened = link_.Open(KlineLinkConfig{.header = KlineHeader::kNone,
                                                     .iso14230_connection = false,
                                                     .baud = 10400,
                                                     .start_byte = ids.start_byte,
                                                     .tester_id = ids.tester_id,
                                                     .target_id = ids.target_id});
            !opened.has_value())
        {
            return opened;
        }
        Info("Testing " + name + " five baud init, please wait...");
        std::ignore = link_.SetP1Max(35ms); // result never checked today
        auto response = link_.FiveBaudInit(kFiveBaudAddress);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        Info("Init response: " + FormatHex(*response));
        const bool j2534 = link_.UsesJ2534();
        const std::optional<KlineHeader> header = FiveBaudHeader(requested, *response, j2534);
        if (!j2534)
        {
            std::ignore = link_.SetP1Max(25ms);
        }
        if (!header.has_value())
        {
            Error(name + " five baud init failed.");
            return Fail(ErrorKind::kBadResponse, name + " five baud init failed");
        }
        std::ignore = link_.SetHeader(*header);
        Info(name + " five baud init succesfully completed.");
        return {};
    }

    Status FastInit()
    {
        const KlineIds ids = IdsFor(ObdProtocol::kIso14230);
        if (auto opened = link_.Open(KlineLinkConfig{.header = KlineHeader::kIso14230,
                                                     .iso14230_connection = true,
                                                     .baud = 10400,
                                                     .start_byte = ids.start_byte,
                                                     .tester_id = ids.tester_id,
                                                     .target_id = ids.target_id});
            !opened.has_value())
        {
            return opened;
        }
        Info("Initialising iso14230 fast init K-Line communications, please wait...");
        if (auto woke = link_.FastInit(bytes::Bytes{kFastInitWakeup}); !woke.has_value())
        {
            return woke; // silent, as today; the caller falls back to five-baud
        }
        auto frame = ReadFrame(kShortRead);
        if (!frame.has_value())
        {
            return std::unexpected(frame.error());
        }
        if (!frame->has_value() || !FastInitAccepted(**frame))
        {
            Error("iso14230 fast init mode failed.");
            return Fail(ErrorKind::kBadResponse, "iso14230 fast init mode failed");
        }
        Info("iso14230 fast init mode succesfully completed.");
        return {};
    }

    Status CanInit()
    {
        if (auto opened = link_.Open(CanLinkConfig{.iso15765 = true,
                                                   .bitrate = 500000,
                                                   .extended_id = false,
                                                   .source_id = kCanSource,
                                                   .destination_id = kCanDestination});
            !opened.has_value())
        {
            return opened;
        }
        Info("Initialising iso15765 CAN communications, please wait...");
        const bytes::Bytes probe = BuildRequest(ObdProtocol::kIso15765, kCanSource, bytes::Bytes{kLiveData, 0x00});
        if (auto written = link_.Write(probe); !written.has_value())
        {
            return std::unexpected(written.error());
        }
        auto frame = link_.Read(kCanInitRead, cancellation_);
        if (!frame.has_value())
        {
            return std::unexpected(frame.error());
        }
        const bytes::Bytes f = frame->value_or(bytes::Bytes{});
        if (f.size() <= 4)
        {
            return Fail(ErrorKind::kBadResponse, "no iso15765 init response");
        }
        if (f[4] == 0x7F)
        {
            // Today's code describes the NRC from offset 3, not 4.
            Error("Wrong response from ECU: " + NrcDescription(bytes::ByteView(f).subspan(3)));
            return Fail(ErrorKind::kBadResponse, "iso15765 init rejected");
        }
        if (f[4] != 0x41)
        {
            Error("Wrong response from ECU: " + FormatHex(f));
            return Fail(ErrorKind::kBadResponse, "iso15765 init wrong response");
        }
        Info("iso15765 init mode succesfully completed.");
        return {};
    }

    Status VehicleInfo()
    {
        Info("Requesting vehicle info, please wait...");
        if (auto slept = clock_.Sleep(kBeforeVehicleInfo, cancellation_); !slept.has_value())
        {
            return slept;
        }
        for (std::size_t page = 0; page < kSupportPages.size(); ++page)
        {
            auto response = Request(kLiveData, kSupportPages.at(page), false);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (!response->empty())
            {
                Info(FormatPidPageLabel(page, *response));
                Info("Supported PIDs: " + FormatSupportedPids(page, *response));
                report_.supported_pids.push_back(SupportedPidPage{page, *response});
            }
            if (auto slept = clock_.Sleep(kBetweenRequests, cancellation_); !slept.has_value())
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
            auto response = Request(item.mode, item.pid, false);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (!response->empty())
            {
                Info(std::string(item.label) + FormatHex(*response));
                if (item.also_text)
                {
                    Info(std::string(item.label) + AsText(*response));
                }
                report_.*item.field = *response;
            }
            if (auto slept = clock_.Sleep(kBetweenRequests, cancellation_); !slept.has_value())
            {
                return slept;
            }
        }
        return {};
    }

    Status ReadDtcs()
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
            auto response = Request(list.mode, std::nullopt, true);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (response->empty())
            {
                return Fail(ErrorKind::kBadResponse, list.missing);
            }
            Info(std::string(list.label) + FormatHex(*response));
            report_.*list.field = DecodeDtcs(*response);
            for (const std::uint16_t code : report_.*list.field)
            {
                Info("DTC: " + DtcDescription(code));
            }
            if (auto slept = clock_.Sleep(kBetweenRequests, cancellation_); !slept.has_value())
            {
                return slept;
            }
        }
        Info("Diagnostic trouble codes succesfully read!");
        return {};
    }

    Status ClearDtcs()
    {
        if (auto read = ReadDtcs(); !read.has_value())
        {
            return read;
        }
        const std::size_t index = ResponseIndex(request_.protocol);
        if (auto written = link_.Write(BuildRequest(request_.protocol, kCanSource, bytes::Bytes{kClearDtcs}));
            !written.has_value())
        {
            return std::unexpected(written.error());
        }
        bool cleared = false;
        while (true)
        {
            auto frame = ReadFrame(kShortRead);
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
                Error("Wrong response from ECU: " + NrcDescription(bytes::ByteView(f).subspan(index)));
                break;
            }
            if (f[index] != (kClearDtcs | 0x40U))
            {
                Error("Wrong response from ECU: " + FormatHex(f));
                break;
            }
            cleared = true;
            break;
        }
        if (!cleared)
        {
            return Fail(ErrorKind::kBadResponse, "clear DTCs not acknowledged");
        }
        report_.cleared = true;
        Info("Diagnostic trouble codes succesfully cleared!");
        return {};
    }

    // Writes one request and collects frames until an empty read. An NRC or
    // wrong response is logged and ends collection with what was gathered.
    Result<bytes::Bytes> Request(std::uint8_t mode, std::optional<std::uint8_t> pid, bool dtc_list)
    {
        bytes::Bytes payload{mode};
        if (pid.has_value())
        {
            payload.push_back(*pid);
        }
        if (auto written = link_.Write(BuildRequest(request_.protocol, kCanSource, payload)); !written.has_value())
        {
            return std::unexpected(written.error());
        }
        bytes::Bytes response;
        while (true)
        {
            auto frame = ReadFrame(kShortRead);
            if (!frame.has_value())
            {
                return std::unexpected(frame.error());
            }
            if (!frame->has_value())
            {
                break;
            }
            const bytes::Bytes& f = **frame;
            const ResponseCheck check = CheckResponse(request_.protocol, f, mode, pid);
            if (check == ResponseCheck::kNrc)
            {
                Error("Wrong response from ECU: " +
                      NrcDescription(bytes::ByteView(f).subspan(ResponseIndex(request_.protocol))));
                break;
            }
            if (check == ResponseCheck::kWrongId)
            {
                Error("Wrong response from ECU: " + FormatHex(f));
                break;
            }
            const bytes::Bytes data =
                dtc_list ? UnframeDtcListResponse(request_.protocol, f) : UnframeDataResponse(request_.protocol, f);
            response.insert(response.end(), data.begin(), data.end());
        }
        return response;
    }

    Result<IDiagnosticLink::OptionalBytes> ReadFrame(std::chrono::milliseconds timeout)
    {
        return link_.UsesJ2534() ? link_.Read(timeout, cancellation_) : link_.ReadObd(timeout, cancellation_);
    }

    void Info(const std::string& message)
    {
        events_.Log(LogLevel::kInfo, message);
    }
    void Error(const std::string& message)
    {
        events_.Log(LogLevel::kError, message);
    }

    DtcRequest request_;
    IDiagnosticLink& link_;
    IClock& clock_;
    const ICancellationToken& cancellation_;
    IEventSink& events_;
    DtcReport report_;
};

} // namespace

Result<DtcReport> RunDtcSession(const DtcRequest& request, IDiagnosticLink& link, IClock& clock,
                                const ICancellationToken& cancellation, IEventSink& events)
{
    return DtcRun(request, link, clock, cancellation, events).Execute();
}

} // namespace fastecu::diagnostics
