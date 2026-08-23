#include "apps/bench/bench_driver.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
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
    const std::string rendered =
        options.json ? format_json(outcome, options.stats) : format_text(outcome, options.stats);
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

bool isEraseHelperUpload(const PreparedStep& step)
{
    return step.spec.id == CommandId::UploadRoutine && step.spec.args.size() == 1 &&
           (step.spec.args.front() == "erase-page" || step.spec.args.front() == "erase-redirect");
}

bool isDestructiveStep(const PreparedStep& step)
{
    return std::ranges::any_of(command_table(), [&step](const CommandSpec& spec)
                               { return spec.id == step.spec.id && spec.destructive; });
}

enum class EraseSequenceState
{
    NeedsHelper,
    NeedsUnlock,
    Ready,
};

constexpr std::string_view kEraseSequenceRequirement =
    "erase requires a successful erase helper upload (built-in upload-routine erase-page or erase-redirect without "
    "--from) followed by a successful unlock, with no intervening destructive or failed step in this session";

EraseSequenceState advanceEraseSequence(EraseSequenceState state, const PreparedStep& step, bool successful)
{
    if (!successful)
    {
        return EraseSequenceState::NeedsHelper;
    }
    if (isEraseHelperUpload(step))
    {
        return EraseSequenceState::NeedsUnlock;
    }
    if (step.spec.id == CommandId::Unlock)
    {
        return state == EraseSequenceState::NeedsUnlock ? EraseSequenceState::Ready : EraseSequenceState::NeedsHelper;
    }
    if (step.spec.id == CommandId::Erase || isDestructiveStep(step))
    {
        return EraseSequenceState::NeedsHelper;
    }
    return state;
}

struct PlanValidationFailure
{
    const PreparedStep *step;
    Error error;
};

std::optional<PlanValidationFailure> validateSessionPlan(const std::vector<const PreparedStep *>& steps)
{
    bool saw_session_step = false;
    EraseSequenceState erase_sequence = EraseSequenceState::NeedsHelper;
    for (const PreparedStep *const step : steps)
    {
        if (step->spec.id == CommandId::Ports)
        {
            continue;
        }
        if (step->spec.id == CommandId::Connect && saw_session_step)
        {
            return PlanValidationFailure{
                step, Error{ErrorKind::InvalidConfig,
                            "connect must be the first non-ports session step and may appear only once"}};
        }
        saw_session_step = true;

        if (step->spec.id == CommandId::Erase && erase_sequence != EraseSequenceState::Ready)
        {
            return PlanValidationFailure{step, Error{ErrorKind::InvalidConfig, std::string(kEraseSequenceRequirement)}};
        }
        erase_sequence = advanceEraseSequence(erase_sequence, *step, true);
    }
    return std::nullopt;
}

struct SessionState
{
    EraseSequenceState erase_sequence = EraseSequenceState::NeedsHelper;
};

int runSteps(IBenchSession& session, IBenchFiles& files, const GlobalOptions& options,
             const std::vector<PreparedStep>& steps, SessionState& state, std::ostream& output,
             std::ostream& diagnostics)
{
    BenchContext context{.session = session, .files = files, .options = options};
    int code = 0;
    for (const PreparedStep& step : steps)
    {
        CommandOutcome outcome;
        if (step.spec.id == CommandId::Erase && state.erase_sequence != EraseSequenceState::Ready)
        {
            outcome = failedOutcome(renderStep(step.spec),
                                    Error{ErrorKind::InvalidConfig, std::string(kEraseSequenceRequirement)});
            if (const Result<double> battery = session.vbatt(); battery.has_value())
            {
                outcome.vbatt = *battery;
            }
        }
        else
        {
            outcome = run_step(context, step);
        }

        state.erase_sequence = advanceEraseSequence(state.erase_sequence, step, outcome.ok);

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

int runPreparedBatch(IBenchEnvironment& environment, IBenchFiles& files, const GlobalOptions& options,
                     const std::vector<PreparedStep>& steps, std::ostream& output, std::ostream& diagnostics)
{
    const bool connect_implicitly =
        !options.no_connect && !steps.empty() && steps.front().spec.id != CommandId::Connect;
    Result<std::reference_wrapper<IBenchSession>> session = environment.session(options, connect_implicitly);
    if (!session.has_value())
    {
        const std::string label = steps.empty() ? "setup" : renderStep(steps.front().spec);
        const CommandOutcome outcome = failedOutcome(label, session.error(), environment.last_setup_traffic());
        emit(options, outcome, output);
        diagnose(outcome, diagnostics);
        return exit_code_for(session.error().kind);
    }
    SessionState state;
    return runSteps(session->get(), files, options, steps, state, output, diagnostics);
}

int runBatch(IBenchEnvironment& environment, IBenchFiles& files, const GlobalOptions& options,
             const std::vector<StepSpec>& steps, std::ostream& output, std::ostream& diagnostics)
{
    std::vector<PreparedStep> prepared_steps;
    prepared_steps.reserve(steps.size());
    for (const StepSpec& step : steps)
    {
        Result<PreparedStep> prepared = prepare_step(files, step);
        if (!prepared.has_value())
        {
            const CommandOutcome outcome = failedOutcome(renderStep(step), prepared.error());
            emit(options, outcome, output);
            diagnose(outcome, diagnostics);
            return exit_code_for(prepared.error().kind);
        }
        prepared_steps.push_back(std::move(*prepared));
    }
    std::vector<const PreparedStep *> plan;
    plan.reserve(prepared_steps.size());
    for (const PreparedStep& step : prepared_steps)
    {
        plan.push_back(&step);
    }
    if (const std::optional<PlanValidationFailure> failure = validateSessionPlan(plan); failure.has_value())
    {
        const CommandOutcome outcome = failedOutcome(renderStep(failure->step->spec), failure->error);
        emit(options, outcome, output);
        diagnose(outcome, diagnostics);
        return exit_code_for(failure->error.kind);
    }
    return runPreparedBatch(environment, files, options, prepared_steps, output, diagnostics);
}

bool isScriptLineGlobalOption(std::string_view token)
{
    return token == "--port" || token == "--json" || token == "--verbose" || token == "--timeout" ||
           token == "--keep-going" || token == "--no-connect" || token == "--vendor-ext" || token == "--stats" ||
           token == "--script";
}

int runScript(IBenchEnvironment& environment, IBenchFiles& files, const GlobalOptions& options, std::istream& input,
              std::ostream& output, std::ostream& diagnostics)
{
    std::vector<std::vector<PreparedStep>> prepared_lines;
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

        std::vector<PreparedStep> prepared_steps;
        prepared_steps.reserve(parsed->steps.size());
        bool line_valid = true;
        for (const StepSpec& step : parsed->steps)
        {
            Result<PreparedStep> prepared = prepare_step(files, step);
            if (prepared.has_value())
            {
                prepared_steps.push_back(std::move(*prepared));
                continue;
            }
            const CommandOutcome outcome =
                failedOutcome(std::format("script line {}: {}", line_number, renderStep(step)), prepared.error());
            emit(options, outcome, output);
            diagnose(outcome, diagnostics);
            if (first_code == 0)
            {
                first_code = exit_code_for(prepared.error().kind);
            }
            line_valid = false;
            break;
        }
        if (!line_valid)
        {
            if (!options.keep_going)
            {
                return first_code;
            }
            continue;
        }

        prepared_lines.push_back(std::move(prepared_steps));
    }

    // A script is a single destructive plan even though each line is executed
    // as a batch. If any line is malformed, do not execute earlier valid lines
    // before discovering it.
    if (first_code != 0)
    {
        return first_code;
    }

    std::vector<const PreparedStep *> plan;
    for (const std::vector<PreparedStep>& line_steps : prepared_lines)
    {
        for (const PreparedStep& step : line_steps)
        {
            plan.push_back(&step);
        }
    }
    if (const std::optional<PlanValidationFailure> failure = validateSessionPlan(plan); failure.has_value())
    {
        const CommandOutcome outcome = failedOutcome(renderStep(failure->step->spec), failure->error);
        emit(options, outcome, output);
        diagnose(outcome, diagnostics);
        return exit_code_for(failure->error.kind);
    }

    std::optional<std::reference_wrapper<IBenchSession>> session;
    SessionState state;
    for (const std::vector<PreparedStep>& prepared_steps : prepared_lines)
    {
        int code = 0;
        if (prepared_steps.size() == 1 && prepared_steps.front().spec.id == CommandId::Ports)
        {
            code = runPorts(environment, options, output, diagnostics);
        }
        else
        {
            if (!session.has_value())
            {
                const bool connect_implicitly = !options.no_connect && !prepared_steps.empty() &&
                                                prepared_steps.front().spec.id != CommandId::Connect;
                Result<std::reference_wrapper<IBenchSession>> opened = environment.session(options, connect_implicitly);
                if (!opened.has_value())
                {
                    const std::string label =
                        prepared_steps.empty() ? "setup" : renderStep(prepared_steps.front().spec);
                    const CommandOutcome outcome =
                        failedOutcome(label, opened.error(), environment.last_setup_traffic());
                    emit(options, outcome, output);
                    diagnose(outcome, diagnostics);
                    if (first_code == 0)
                    {
                        first_code = exit_code_for(opened.error().kind);
                    }
                    return first_code;
                }
                session = *opened;
            }
            code = runSteps(session->get(), files, options, prepared_steps, state, output, diagnostics);
        }
        if (code != 0)
        {
            if (first_code == 0)
            {
                first_code = code;
            }
            if (!options.keep_going)
            {
                return first_code;
            }
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
