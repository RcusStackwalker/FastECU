#include <QCoreApplication>

#include <csignal>
#include <cstddef>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
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

using fastecu::Error;
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
using fastecu::bench::IBenchSession;
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

// Enumerates serial ports without ever constructing a transport or session --
// the invariant `ports` promises. Callers (both the top-level shortcut and
// the --script - loop) must confirm the parsed step really is a solitary
// `ports` before calling this.
int handlePorts()
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

// Opens the transport and, unless --no-connect, runs the 0x10/0x27 handshake
// -- but only the first time get() is called. Exists so a --script - run
// that only ever sends `ports` lines never touches hardware: the transport
// cannot be opened eagerly before the loop starts because stdin is read one
// line at a time, so opening happens lazily on the first line that actually
// needs a session instead.
class LazySession
{
  public:
    LazySession(GlobalOptions options, const fastecu::ICancellationToken& cancellation)
        : options_(std::move(options)), cancellation_(cancellation)
    {
    }

    Result<std::reference_wrapper<IBenchSession>> get()
    {
        if (session_.has_value())
        {
            return std::ref(static_cast<IBenchSession&>(*session_));
        }

        fastecu::flash::DesktopCanTransportConfig transport_config;
        transport_config.port_name = options_.port_name;
        constexpr fastecu::flash::Iso15765Config kCanConfig{
            .bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false};

        Result<std::unique_ptr<fastecu::flash::ICanFlashTransport>> transport =
            fastecu::flash::open_desktop_can_flash_transport(transport_config, kCanConfig);
        if (!transport.has_value())
        {
            return std::unexpected(transport.error());
        }

        session_.emplace(std::move(*transport), kCanConfig.request_id, kCanConfig.response_id, clock_, events_,
                         cancellation_);

        if (!options_.no_connect)
        {
            const Status connected = session_->connect();
            if (!connected.has_value())
            {
                const Error error = connected.error();
                session_.reset();
                return std::unexpected(error);
            }
        }
        return std::ref(static_cast<IBenchSession&>(*session_));
    }

  private:
    GlobalOptions options_;
    const fastecu::ICancellationToken& cancellation_;
    StderrEventSink events_;
    QtClock clock_;
    std::optional<BenchSession> session_;
};

// Runs one already-parsed batch of steps against `context`, printing each
// outcome to stdout. Returns the exit code for the first failure, or 0 once
// every step has run (or --keep-going carried through all of them) -- never
// overwritten by a later failure, so an agent branching on the exit code
// sees the same failure stdout showed first.
int runSteps(BenchContext& context, const std::vector<StepSpec>& steps)
{
    int code = 0;
    for (const StepSpec& step : steps)
    {
        const Result<CommandOutcome> result = fastecu::bench::run_step(context, step);
        if (result.has_value())
        {
            std::cout << render(context.options, *result) << "\n";
            if (result->ok)
            {
                continue;
            }
            if (code == 0)
            {
                code = result->error_kind.has_value() ? fastecu::bench::exit_code_for(*result->error_kind)
                                                      : fastecu::bench::exit_code_for(ErrorKind::Internal);
            }
        }
        else
        {
            const CommandOutcome failed{.step = renderStep(step),
                                        .ok = false,
                                        .error_kind = result.error().kind,
                                        .error_detail = result.error().detail};
            std::cout << render(context.options, failed) << "\n";
            if (code == 0)
            {
                code = fastecu::bench::exit_code_for(result.error().kind);
            }
        }
        if (!context.options.keep_going)
        {
            return code;
        }
    }
    return code;
}

// Drives the --script - loop: one line of stdin at a time, each re-parsed as
// its own command line and executed in the one long-lived session `lazy`
// owns. A `ports` line is served without touching `lazy` at all, so a script
// that never sends a real command never opens the transport.
int runScript(LazySession& lazy, BenchFiles& files, const GlobalOptions& options)
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
            if (code == 0)
            {
                code = fastecu::bench::exit_code_for(line_parsed.error().kind);
            }
            if (!options.keep_going)
            {
                return code;
            }
            continue;
        }

        if (line_parsed->steps.size() == 1 && line_parsed->steps.front().id == CommandId::Ports)
        {
            const int ports_code = handlePorts();
            if (ports_code != 0)
            {
                if (code == 0)
                {
                    code = ports_code;
                }
                if (!options.keep_going)
                {
                    return code;
                }
            }
            continue;
        }

        Result<std::reference_wrapper<IBenchSession>> session = lazy.get();
        if (!session.has_value())
        {
            std::cerr << session.error().detail << "\n";
            // Not recoverable within this script: nothing after this line can
            // succeed either, so --keep-going does not apply here.
            return code == 0 ? fastecu::bench::exit_code_for(session.error().kind) : code;
        }
        BenchContext context{.session = session->get(), .files = files, .options = options};
        const int step_code = runSteps(context, line_parsed->steps);
        if (step_code != 0)
        {
            if (code == 0)
            {
                code = step_code;
            }
            if (!options.keep_going)
            {
                return code;
            }
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

    // `ports` never opens a device. bench_args guarantees it is never mixed
    // with other steps in one chain, so a non-script top-level parse
    // consisting of the single Ports step is served here before any
    // transport or session is constructed -- see bench_commands.cpp's
    // CommandId::Ports case.
    if (!parsed->options.script_stdin && parsed->steps.size() == 1 && parsed->steps.front().id == CommandId::Ports)
    {
        return handlePorts();
    }

    QtCancellationToken cancellation;
    g_cancellation = &cancellation;
    std::signal(SIGINT, handleSigint);

    BenchFiles files;
    LazySession lazy(parsed->options, cancellation);

    if (parsed->options.script_stdin)
    {
        return runScript(lazy, files, parsed->options);
    }

    // Every other shape reaching here needs a real session: bench_args
    // rejects Ports mixed with anything else, so a chain that got past the
    // shortcut above contains no Ports step at all.
    Result<std::reference_wrapper<IBenchSession>> session = lazy.get();
    if (!session.has_value())
    {
        std::cerr << session.error().detail << "\n";
        return fastecu::bench::exit_code_for(session.error().kind);
    }
    BenchContext context{.session = session->get(), .files = files, .options = parsed->options};
    return runSteps(context, parsed->steps);
}
