#include "apps/bench/bench_driver.h"

#include <algorithm>
#include <concepts>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "apps/bench/bench_args.h"
#include "apps/bench/bench_commands.h"
#include "apps/bench/bench_format.h"

namespace fastecu::bench
{
namespace
{

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

// Where every outcome goes: one rendered line on `output`, the failure detail
// on `diagnostics`. Bundled because all of the driver writes both, and the
// exit code of a failure is a property of the same report.
struct Reporter
{
    const GlobalOptions& options;
    std::ostream& output;
    std::ostream& diagnostics;

    void report(const CommandOutcome& outcome) const
    {
        const std::string rendered =
            options.json ? format_json(outcome, options.stats) : format_text(outcome, options.stats);
        output << rendered;
        if (rendered.empty() || rendered.back() != '\n')
        {
            output << '\n';
        }
        if (!outcome.ok && !outcome.error_detail.empty())
        {
            diagnostics << outcome.error_detail << '\n';
        }
    }

    // Reports a failure and returns the exit code it maps to.
    int fail(std::string step, const Error& error, const TrafficEvidence& traffic = {}) const
    {
        report(failedOutcome(std::move(step), error, traffic));
        return exit_code_for(error.kind);
    }
};

// A run reports every failure it reaches but exits with the first one.
void ratchet(int& first_code, int code)
{
    if (first_code == 0)
    {
        first_code = code;
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

// Checks what the whole plan implies before any of it runs: a script spans
// several lines but is still one session, so `connect` placement and the
// erase prerequisite chain are validated across line boundaries.
template <std::ranges::input_range Steps>
    requires std::convertible_to<std::ranges::range_reference_t<Steps>, const PreparedStep&>
std::optional<PlanValidationFailure> validateSessionPlan(Steps&& steps)
{
    bool saw_session_step = false;
    EraseSequenceState erase_sequence = EraseSequenceState::NeedsHelper;
    for (const PreparedStep& step : steps)
    {
        if (step.spec.id == CommandId::Ports)
        {
            continue;
        }
        if (step.spec.id == CommandId::Connect && saw_session_step)
        {
            return PlanValidationFailure{
                &step, Error{ErrorKind::InvalidConfig,
                             "connect must be the first non-ports session step and may appear only once"}};
        }
        saw_session_step = true;

        if (step.spec.id == CommandId::Erase && erase_sequence != EraseSequenceState::Ready)
        {
            return PlanValidationFailure{&step,
                                         Error{ErrorKind::InvalidConfig, std::string(kEraseSequenceRequirement)}};
        }
        erase_sequence = advanceEraseSequence(erase_sequence, step, true);
    }
    return std::nullopt;
}

struct SessionState
{
    EraseSequenceState erase_sequence = EraseSequenceState::NeedsHelper;
};

int runSteps(IBenchSession& session, IBenchFiles& files, const Reporter& reporter, std::span<const PreparedStep> steps,
             SessionState& state)
{
    BenchContext context{.session = session, .files = files, .options = reporter.options};
    int code = 0;
    for (const PreparedStep& step : steps)
    {
        CommandOutcome outcome;
        if (step.spec.id == CommandId::Erase && state.erase_sequence != EraseSequenceState::Ready)
        {
            outcome = failedOutcome(render_step(step.spec),
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

        reporter.report(outcome);
        if (outcome.ok)
        {
            continue;
        }
        ratchet(code, exit_code_for(outcome.error_kind.value_or(ErrorKind::Internal)));
        if (!reporter.options.keep_going)
        {
            break;
        }
    }
    return code;
}

int runPorts(IBenchEnvironment& environment, const Reporter& reporter)
{
    const Result<std::vector<std::string>> ports = environment.list_ports(reporter.options);
    if (!ports.has_value())
    {
        return reporter.fail("ports", ports.error());
    }

    if (!reporter.options.json)
    {
        for (const std::string& port : *ports)
        {
            reporter.output << port << '\n';
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
    reporter.report(CommandOutcome{.step = "ports", .note = std::move(note)});
    return 0;
}

struct PreparationFailure
{
    const StepSpec *step;
    Error error;
};

// Loads file-backed payloads and validates every step, stopping at the first
// failure. Naming that step is left to the caller: a script prefixes its line
// number.
std::expected<std::vector<PreparedStep>, PreparationFailure> prepareSteps(IBenchFiles& files,
                                                                          std::span<const StepSpec> specs)
{
    std::vector<PreparedStep> prepared;
    prepared.reserve(specs.size());
    for (const StepSpec& spec : specs)
    {
        Result<PreparedStep> step = prepare_step(files, spec);
        if (!step.has_value())
        {
            return std::unexpected(PreparationFailure{.step = &spec, .error = step.error()});
        }
        prepared.push_back(std::move(*step));
    }
    return prepared;
}

// What a failure before the first step ran is called.
std::string planLabel(std::span<const PreparedStep> steps)
{
    return steps.empty() ? std::string("setup") : render_step(steps.front().spec);
}

// The one session every step shares. Connecting is implicit unless the plan
// opens with an explicit `connect`.
Result<std::reference_wrapper<IBenchSession>> openSession(IBenchEnvironment& environment, const GlobalOptions& options,
                                                          std::span<const PreparedStep> steps)
{
    const bool connect_implicitly =
        !options.no_connect && !steps.empty() && steps.front().spec.id != CommandId::Connect;
    return environment.session(options, connect_implicitly);
}

int runBatch(IBenchEnvironment& environment, IBenchFiles& files, const Reporter& reporter,
             std::span<const StepSpec> steps)
{
    const std::expected<std::vector<PreparedStep>, PreparationFailure> prepared = prepareSteps(files, steps);
    if (!prepared.has_value())
    {
        return reporter.fail(render_step(*prepared.error().step), prepared.error().error);
    }
    if (const std::optional<PlanValidationFailure> failure = validateSessionPlan(*prepared); failure.has_value())
    {
        return reporter.fail(render_step(failure->step->spec), failure->error);
    }

    const Result<std::reference_wrapper<IBenchSession>> session = openSession(environment, reporter.options, *prepared);
    if (!session.has_value())
    {
        return reporter.fail(planLabel(*prepared), session.error(), environment.last_setup_traffic());
    }
    SessionState state;
    return runSteps(session->get(), files, reporter, *prepared, state);
}

struct ScriptPlan
{
    std::vector<std::vector<PreparedStep>> lines;
    // Non-zero once a line failed to parse or prepare, which forbids executing
    // any line at all.
    int code = 0;
};

// Reads the whole script and prepares every line before any of them runs.
ScriptPlan prepareScript(IBenchFiles& files, const Reporter& reporter, std::istream& input)
{
    ScriptPlan plan;
    // Reports the failure; true means stop reading, false means keep going to
    // show the operator every bad line before refusing to run any of them.
    const auto lineFailed = [&](std::string label, const Error& error)
    {
        ratchet(plan.code, reporter.fail(std::move(label), error));
        return !reporter.options.keep_going;
    };

    std::string line;
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

        const std::string line_label = std::format("script line {}", line_number);

        if (const auto forbidden = std::ranges::find_if(tokens, [](std::string_view token)
                                                        { return find_global_option(token) != nullptr; });
            forbidden != tokens.end())
        {
            const Error error{ErrorKind::InvalidConfig,
                              std::format("script-line global option {} is not allowed; put it on the outer "
                                          "--script invocation",
                                          *forbidden)};
            if (lineFailed(line_label, error))
            {
                return plan;
            }
            continue;
        }

        const std::vector<std::string_view> line_args(tokens.begin(), tokens.end());
        const Result<ParsedCommandLine> parsed = parse_command_line(line_args);
        if (!parsed.has_value())
        {
            if (lineFailed(line_label, parsed.error()))
            {
                return plan;
            }
            continue;
        }

        std::expected<std::vector<PreparedStep>, PreparationFailure> prepared = prepareSteps(files, parsed->steps);
        if (!prepared.has_value())
        {
            if (lineFailed(std::format("{}: {}", line_label, render_step(*prepared.error().step)),
                           prepared.error().error))
            {
                return plan;
            }
            continue;
        }

        plan.lines.push_back(std::move(*prepared));
    }
    return plan;
}

// parse_command_line rejects chaining `ports`, so a line naming it holds no
// other step and needs no session.
bool isPortsLine(std::span<const PreparedStep> steps)
{
    return !steps.empty() && steps.front().spec.id == CommandId::Ports;
}

int runScriptLines(IBenchEnvironment& environment, IBenchFiles& files, const Reporter& reporter,
                   std::span<const std::vector<PreparedStep>> lines)
{
    // Opened at the first line that needs it and shared by the rest, along
    // with the erase prerequisite state the lines build up between them.
    std::optional<std::reference_wrapper<IBenchSession>> session;
    SessionState state;
    int first_code = 0;
    for (const std::vector<PreparedStep>& steps : lines)
    {
        int code = 0;
        if (isPortsLine(steps))
        {
            code = runPorts(environment, reporter);
        }
        else
        {
            if (!session.has_value())
            {
                const Result<std::reference_wrapper<IBenchSession>> opened =
                    openSession(environment, reporter.options, steps);
                if (!opened.has_value())
                {
                    return reporter.fail(planLabel(steps), opened.error(), environment.last_setup_traffic());
                }
                session = *opened;
            }
            code = runSteps(session->get(), files, reporter, steps, state);
        }

        ratchet(first_code, code);
        if (first_code != 0 && !reporter.options.keep_going)
        {
            return first_code;
        }
    }
    return first_code;
}

int runScript(IBenchEnvironment& environment, IBenchFiles& files, const Reporter& reporter, std::istream& input)
{
    const ScriptPlan plan = prepareScript(files, reporter, input);
    // A script is a single destructive plan even though each line is executed
    // as a batch: a malformed line 7 must not be discovered after line 1 has
    // already erased, so one bad line cancels the whole script.
    if (plan.code != 0)
    {
        return plan.code;
    }
    if (const std::optional<PlanValidationFailure> failure = validateSessionPlan(plan.lines | std::views::join);
        failure.has_value())
    {
        return reporter.fail(render_step(failure->step->spec), failure->error);
    }
    return runScriptLines(environment, files, reporter, plan.lines);
}

} // namespace

int run_cli(IBenchEnvironment& environment, IBenchFiles& files, std::span<const std::string_view> args,
            std::istream& input, std::ostream& output, std::ostream& diagnostics)
{
    const Result<ParsedCommandLine> parsed = parse_command_line(args);
    if (!parsed.has_value())
    {
        if (std::ranges::find(args, "--json") != args.end())
        {
            GlobalOptions options;
            options.json = true;
            return Reporter{options, output, diagnostics}.fail("command line", parsed.error());
        }
        diagnostics << parsed.error().detail << '\n';
        return exit_code_for(parsed.error().kind);
    }

    const Reporter reporter{parsed->options, output, diagnostics};
    if (parsed->options.script_stdin)
    {
        return runScript(environment, files, reporter, input);
    }
    if (parsed->steps.size() == 1 && parsed->steps.front().id == CommandId::Ports)
    {
        return runPorts(environment, reporter);
    }
    return runBatch(environment, files, reporter, parsed->steps);
}

} // namespace fastecu::bench
