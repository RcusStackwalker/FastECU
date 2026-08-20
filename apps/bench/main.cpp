#include <QCoreApplication>

#include <csignal>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "apps/bench/bench_args.h"
#include "apps/bench/bench_commands.h"
#include "apps/bench/bench_files.h"
#include "apps/bench/bench_format.h"
#include "apps/bench/bench_session.h"
#include "apps/bench/bench_types.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/event_sink.h"
#include "src/platform/desktop/common/ports/qt_cancellation_token.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/transport/desktop_transport_factory.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::Result;
using fastecu::Status;
using fastecu::bench::BenchContext;
using fastecu::bench::BenchFiles;
using fastecu::bench::BenchSession;
using fastecu::bench::CommandId;
using fastecu::bench::CommandOutcome;
using fastecu::bench::CommandSpec;
using fastecu::bench::GlobalOptions;
using fastecu::bench::ParsedCommandLine;
using fastecu::bench::StepSpec;

// Routes backend log/notice events to stderr so stdout stays pure step
// output, matching bench_format.h's "emitted on stdout; all logging goes to
// stderr" contract.
class StderrEventSink final : public fastecu::IEventSink
{
  public:
    void log(fastecu::LogLevel /*level*/, std::string_view message) override
    {
        std::cerr << message << "\n";
    }
    void progress(int /*done*/, int /*total*/) override
    {
    }
    void notice(std::string_view message) override
    {
        std::cerr << message << "\n";
    }
};

QtCancellationToken *g_cancellation = nullptr;

extern "C" void handleSigint(int /*signal*/)
{
    if (g_cancellation != nullptr)
    {
        g_cancellation->cancel();
    }
}

// Mirrors bench_commands.cpp's private renderStep: command name plus its
// arguments. Used only to label a step that failed before run_step produced
// an outcome of its own.
std::string renderStep(const StepSpec& step)
{
    std::string text;
    for (const CommandSpec& spec : fastecu::bench::command_table())
    {
        if (spec.id == step.id)
        {
            text = std::string(spec.name);
            break;
        }
    }
    for (const std::string& arg : step.args)
    {
        text += ' ';
        text += arg;
    }
    return text;
}

std::string render(const GlobalOptions& options, const CommandOutcome& outcome)
{
    return options.json ? fastecu::bench::format_json(outcome) : fastecu::bench::format_text(outcome);
}

// Runs one already-parsed batch of steps against `context`, printing each
// outcome to stdout. Returns the exit code for the first failure, or 0 once
// every step has run (or --keep-going carried through all of them).
int runSteps(BenchContext& context, const std::vector<StepSpec>& steps)
{
    int code = 0;
    for (const StepSpec& step : steps)
    {
        const Result<CommandOutcome> result = fastecu::bench::run_step(context, step);
        if (result.has_value())
        {
            std::cout << render(context.options, *result);
            if (result->ok)
            {
                continue;
            }
            code = result->error_kind.has_value() ? fastecu::bench::exit_code_for(*result->error_kind)
                                                  : fastecu::bench::exit_code_for(ErrorKind::Internal);
        }
        else
        {
            const CommandOutcome failed{.step = renderStep(step),
                                        .ok = false,
                                        .error_kind = result.error().kind,
                                        .error_detail = result.error().detail};
            std::cout << render(context.options, failed);
            code = fastecu::bench::exit_code_for(result.error().kind);
        }
        if (!context.options.keep_going)
        {
            return code;
        }
    }
    return code;
}

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

    const Result<ParsedCommandLine> parsed = fastecu::bench::parse_command_line(args);
    if (!parsed.has_value())
    {
        std::cerr << parsed.error().detail << "\n";
        return fastecu::bench::exit_code_for(parsed.error().kind);
    }

    // `ports` never opens a device, so it is handled before any transport or
    // session is constructed -- see bench_commands.cpp's CommandId::Ports case.
    if (parsed->steps.size() == 1 && parsed->steps.front().id == CommandId::Ports)
    {
        const Result<std::vector<std::string>> ports =
            fastecu::flash::list_desktop_serial_ports(fastecu::flash::DesktopCanTransportConfig{});
        if (!ports.has_value())
        {
            std::cerr << ports.error().detail << "\n";
            return fastecu::bench::exit_code_for(ports.error().kind);
        }
        for (const std::string& name : *ports)
        {
            std::cout << name << "\n";
        }
        return 0;
    }

    QtCancellationToken cancellation;
    g_cancellation = &cancellation;
    std::signal(SIGINT, handleSigint);

    fastecu::flash::DesktopCanTransportConfig transport_config;
    transport_config.port_name = parsed->options.port_name;
    constexpr fastecu::flash::Iso15765Config kCanConfig{
        .bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false};

    Result<std::unique_ptr<fastecu::flash::ICanFlashTransport>> transport =
        fastecu::flash::open_desktop_can_flash_transport(transport_config, kCanConfig);
    if (!transport.has_value())
    {
        std::cerr << transport.error().detail << "\n";
        return fastecu::bench::exit_code_for(transport.error().kind);
    }

    StderrEventSink events;
    QtClock clock;
    BenchSession session(std::move(*transport), kCanConfig.request_id, kCanConfig.response_id, clock, events,
                         cancellation);
    BenchFiles files;
    BenchContext context{.session = session, .files = files, .options = parsed->options};

    if (!parsed->options.no_connect)
    {
        const Status connected = session.connect();
        if (!connected.has_value())
        {
            std::cerr << connected.error().detail << "\n";
            return fastecu::bench::exit_code_for(connected.error().kind);
        }
    }

    if (parsed->options.script_stdin)
    {
        std::string line;
        int code = 0;
        while (std::getline(std::cin, line))
        {
            std::istringstream tokenizer(line);
            const std::vector<std::string> tokens{std::istream_iterator<std::string>{tokenizer},
                                                  std::istream_iterator<std::string>{}};
            if (tokens.empty())
            {
                continue;
            }
            const std::vector<std::string_view> line_args(tokens.begin(), tokens.end());
            const Result<ParsedCommandLine> line_parsed = fastecu::bench::parse_command_line(line_args);
            if (!line_parsed.has_value())
            {
                std::cerr << line_parsed.error().detail << "\n";
                code = fastecu::bench::exit_code_for(line_parsed.error().kind);
                if (!parsed->options.keep_going)
                {
                    return code;
                }
                continue;
            }
            const int step_code = runSteps(context, line_parsed->steps);
            if (step_code != 0)
            {
                code = step_code;
                if (!parsed->options.keep_going)
                {
                    return code;
                }
            }
        }
        return code;
    }

    return runSteps(context, parsed->steps);
}
