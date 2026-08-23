#include <QCoreApplication>

#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "apps/bench/bench_driver.h"
#include "apps/bench/bench_files.h"
#include "apps/bench/bench_session.h"
#include "apps/bench/sigint_cancellation_token.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/event_sink.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/transport/desktop_transport_factory.h"

namespace
{

using fastecu::Error;
using fastecu::Result;
using fastecu::Status;
using fastecu::bench::BenchSession;
using fastecu::bench::GlobalOptions;
using fastecu::bench::IBenchEnvironment;
using fastecu::bench::IBenchSession;
using fastecu::bench::SigintCancellationToken;
using fastecu::bench::TrafficEvidence;

class StderrEventSink final : public fastecu::IEventSink
{
  public:
    void log(fastecu::LogLevel /*level*/, std::string_view message) override
    {
        std::cerr << message << '\n';
    }
    void progress(int /*done*/, int /*total*/) override
    {
    }
    void notice(std::string_view message) override
    {
        std::cerr << message << '\n';
    }
};

class DesktopBenchEnvironment final : public IBenchEnvironment
{
  public:
    explicit DesktopBenchEnvironment(const fastecu::ICancellationToken& cancellation) : cancellation_(cancellation)
    {
    }

    Result<std::vector<std::string>> list_ports(const GlobalOptions&) override
    {
        return fastecu::flash::list_desktop_serial_ports(fastecu::flash::DesktopCanTransportConfig{});
    }

    Result<std::reference_wrapper<IBenchSession>> session(const GlobalOptions& options,
                                                          bool connect_implicitly) override
    {
        if (session_.has_value())
        {
            return std::ref(static_cast<IBenchSession&>(*session_));
        }

        last_setup_traffic_ = {};
        fastecu::flash::DesktopCanTransportConfig transport_config;
        transport_config.port_name = options.port_name;
        constexpr fastecu::flash::Iso15765Config kCanConfig{
            .bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false};
        Result<std::unique_ptr<fastecu::flash::ICanFlashTransport>> transport =
            fastecu::flash::open_desktop_can_flash_transport(transport_config, kCanConfig);
        if (!transport.has_value())
        {
            return std::unexpected(transport.error());
        }

        session_.emplace(std::move(*transport), kCanConfig.request_id, kCanConfig.response_id, clock_, events_,
                         cancellation_, options.vendor_ext);
        if (connect_implicitly)
        {
            const Status connected = session_->connect();
            last_setup_traffic_ = session_->last_traffic();
            if (!connected.has_value())
            {
                const Error error = connected.error();
                session_.reset();
                return std::unexpected(error);
            }
        }
        return std::ref(static_cast<IBenchSession&>(*session_));
    }

    const TrafficEvidence& last_setup_traffic() const override
    {
        return last_setup_traffic_;
    }

  private:
    const fastecu::ICancellationToken& cancellation_;
    StderrEventSink events_;
    QtClock clock_;
    std::optional<BenchSession> session_;
    TrafficEvidence last_setup_traffic_;
};

} // namespace

int main(int argc, char *argv[])
{
    const QCoreApplication app(argc, argv);

    std::vector<std::string_view> args;
    args.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : std::size_t{0});
    for (int index = 1; index < argc; ++index)
    {
        args.emplace_back(argv[index]);
    }

    SigintCancellationToken cancellation;

    DesktopBenchEnvironment environment(cancellation);
    fastecu::bench::BenchFiles files;
    return fastecu::bench::run_cli(environment, files, args, std::cin, std::cout, std::cerr);
}
