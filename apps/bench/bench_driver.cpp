#include "apps/bench/bench_driver.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <sstream>

#include "apps/bench/bench_commands.h"
#include "apps/bench/bench_format.h"

namespace fastecu::bench
{
namespace
{

std::string renderStep(const StepSpec& step)
{
    std::string text;
    for (const CommandSpec& spec : command_table())
    {
        if (spec.id == step.id)
        {
            text = spec.name;
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

void copyTraffic(CommandOutcome& outcome, const TrafficEvidence& traffic)
{
    outcome.exchange_count = traffic.exchange_count;
    outcome.tx = traffic.tx;
    outcome.rx = traffic.rx;
    outcome.last_tx = traffic.last_tx;
    outcome.last_rx = traffic.last_rx;
    outcome.elapsed_ms = traffic.elapsed_ms;
}

CommandOutcome failedOutcome(std::string step, const Error& error, const TrafficEvidence& traffic = {})
{
    CommandOutcome outcome{
        .step = std::move(step), .ok = false, .error_kind = error.kind, .error_detail = error.detail};
    copyTraffic(outcome, traffic);
    return outcome;
}

void emit(const GlobalOptions& options, const CommandOutcome& outcome, std::ostream& output)
{
    const std::string rendered = options.json ? format_json(outcome) : format_text(outcome);
    output << rendered;
    if (rendered.empty() || rendered.back() != '\n')
    {
        output << '\n';
    }
}

void diagnose(const CommandOutcome& outcome, std::ostream& diagnostics)
{
    if (!outcome.ok && !outcome.error_detail.empty())
    {
        diagnostics << outcome.error_detail << '\n';
    }
}

int runSteps(IBenchSession& session, IBenchFiles& files, const GlobalOptions& options,
             const std::vector<StepSpec>& steps, std::ostream& output, std::ostream& diagnostics)
{
    BenchContext context{.session = session, .files = files, .options = options};
    int code = 0;
    for (const StepSpec& step : steps)
    {
        const CommandOutcome outcome = run_step(context, step);
        emit(options, outcome, output);
        diagnose(outcome, diagnostics);
        if (outcome.ok)
        {
            continue;
        }
        if (code == 0)
        {
            code = exit_code_for(outcome.error_kind.value_or(ErrorKind::Internal));
        }
        if (!options.keep_going)
        {
            break;
        }
    }
    return code;
}

int runPorts(IBenchEnvironment& environment, const GlobalOptions& options, std::ostream& output,
             std::ostream& diagnostics)
{
    const Result<std::vector<std::string>> ports = environment.list_ports(options);
    if (!ports.has_value())
    {
        const CommandOutcome outcome = failedOutcome("ports", ports.error());
        emit(options, outcome, output);
        diagnose(outcome, diagnostics);
        return exit_code_for(ports.error().kind);
    }

    if (!options.json)
    {
        for (const std::string& port : *ports)
        {
            output << port << '\n';
        }
        return 0;
    }

    std::string note = "ports=";
    for (std::size_t index = 0; index < ports->size(); ++index)
    {
        if (index > 0)
        {
            note += ',';
        }
        note += (*ports)[index];
    }
    emit(options, CommandOutcome{.step = "ports", .note = std::move(note)}, output);
    return 0;
}

int runBatch(IBenchEnvironment& environment, IBenchFiles& files, const GlobalOptions& options,
             const std::vector<StepSpec>& steps, std::ostream& output, std::ostream& diagnostics)
{
    const bool connect_implicitly = !options.no_connect && !steps.empty() && steps.front().id != CommandId::Connect;
    Result<std::reference_wrapper<IBenchSession>> session = environment.session(options, connect_implicitly);
    if (!session.has_value())
    {
        const std::string label = steps.empty() ? "setup" : renderStep(steps.front());
        const CommandOutcome outcome = failedOutcome(label, session.error(), environment.last_setup_traffic());
        emit(options, outcome, output);
        diagnose(outcome, diagnostics);
        return exit_code_for(session.error().kind);
    }
    return runSteps(session->get(), files, options, steps, output, diagnostics);
}

bool isScriptLineGlobalOption(std::string_view token)
{
    return token == "--port" || token == "--json" || token == "--verbose" || token == "--timeout" ||
           token == "--keep-going" || token == "--no-connect" || token == "--script";
}

int runScript(IBenchEnvironment& environment, IBenchFiles& files, const GlobalOptions& options, std::istream& input,
              std::ostream& output, std::ostream& diagnostics)
{
    std::string line;
    int first_code = 0;
    std::size_t line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        std::istringstream tokenizer(line);
        const std::vector<std::string> tokens{std::istream_iterator<std::string>{tokenizer},
                                              std::istream_iterator<std::string>{}};
        if (tokens.empty())
        {
            continue;
        }

        const auto forbidden = std::ranges::find_if(tokens, isScriptLineGlobalOption);
        if (forbidden != tokens.end())
        {
            const Error error{ErrorKind::InvalidConfig,
                              std::format("script-line global option {} is not allowed; put it on the outer "
                                          "--script invocation",
                                          *forbidden)};
            const CommandOutcome outcome = failedOutcome(std::format("script line {}", line_number), error);
            emit(options, outcome, output);
            diagnose(outcome, diagnostics);
            if (first_code == 0)
            {
                first_code = exit_code_for(error.kind);
            }
            if (!options.keep_going)
            {
                return first_code;
            }
            continue;
        }

        const std::vector<std::string_view> line_args(tokens.begin(), tokens.end());
        const Result<ParsedCommandLine> parsed = parse_command_line(line_args);
        if (!parsed.has_value())
        {
            const CommandOutcome outcome = failedOutcome(std::format("script line {}", line_number), parsed.error());
            emit(options, outcome, output);
            diagnose(outcome, diagnostics);
            if (first_code == 0)
            {
                first_code = exit_code_for(parsed.error().kind);
            }
            if (!options.keep_going)
            {
                return first_code;
            }
            continue;
        }

        int code = 0;
        if (parsed->steps.size() == 1 && parsed->steps.front().id == CommandId::Ports)
        {
            code = runPorts(environment, options, output, diagnostics);
        }
        else
        {
            code = runBatch(environment, files, options, parsed->steps, output, diagnostics);
        }
        if (code != 0 && first_code == 0)
        {
            first_code = code;
        }
        if (code != 0 && !options.keep_going)
        {
            return first_code;
        }
    }
    return first_code;
}

} // namespace

int run_cli(IBenchEnvironment& environment, IBenchFiles& files, std::span<const std::string_view> args,
            std::istream& input, std::ostream& output, std::ostream& diagnostics)
{
    const Result<ParsedCommandLine> parsed = parse_command_line(args);
    if (!parsed.has_value())
    {
        diagnostics << parsed.error().detail << '\n';
        if (std::ranges::find(args, "--json") != args.end())
        {
            GlobalOptions options;
            options.json = true;
            emit(options, failedOutcome("command line", parsed.error()), output);
        }
        return exit_code_for(parsed.error().kind);
    }

    if (parsed->options.script_stdin)
    {
        return runScript(environment, files, parsed->options, input, output, diagnostics);
    }
    if (parsed->steps.size() == 1 && parsed->steps.front().id == CommandId::Ports)
    {
        return runPorts(environment, parsed->options, output, diagnostics);
    }
    return runBatch(environment, files, parsed->options, parsed->steps, output, diagnostics);
}

} // namespace fastecu::bench
