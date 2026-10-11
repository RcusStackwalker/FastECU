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

void CopyTraffic(CommandOutcome& outcome, const TrafficEvidence& traffic)
{
    outcome.exchange_count = traffic.exchange_count;
    outcome.tx = traffic.tx;
    outcome.rx = traffic.rx;
    outcome.last_tx = traffic.last_tx;
    outcome.last_rx = traffic.last_rx;
    outcome.elapsed_ms = traffic.elapsed_ms;
}

CommandOutcome FailedOutcome(std::string step, const Error& error, const TrafficEvidence& traffic = {})
{
    CommandOutcome outcome{
        .step = std::move(step), .ok = false, .error_kind = error.kind, .error_detail = error.detail};
    CopyTraffic(outcome, traffic);
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

    void Report(const CommandOutcome& outcome) const
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
    int Fail(std::string step, const Error& error, const TrafficEvidence& traffic = {}) const
    {
        Report(FailedOutcome(std::move(step), error, traffic));
        return exit_code_for(error.kind);
    }
};

// A run reports every failure it reaches but exits with the first one.
void Ratchet(int& first_code, int code)
{
    if (first_code == 0)
    {
        first_code = code;
    }
}

bool IsDestructiveStep(const PreparedStep& step)
{
    const CommandSpec *const spec = find_command(step.spec.id);
    return spec != nullptr && spec->destructive;
}

enum class EraseSequenceState
{
    kNeedsHelper,
    kNeedsUnlock,
    kReady,
};

constexpr std::string_view kEraseSequenceRequirement =
    "erase requires a successful erase helper upload (built-in upload-routine erase-page or erase-redirect without "
    "--from) followed by a successful unlock, with no intervening destructive or failed step in this session";

EraseSequenceState AdvanceEraseSequence(EraseSequenceState state, const PreparedStep& step, bool successful)
{
    if (!successful)
    {
        return EraseSequenceState::kNeedsHelper;
    }
    if (step.provides_erase_helper)
    {
        return EraseSequenceState::kNeedsUnlock;
    }
    if (step.spec.id == CommandId::Unlock)
    {
        return state == EraseSequenceState::kNeedsUnlock ? EraseSequenceState::kReady
                                                         : EraseSequenceState::kNeedsHelper;
    }
    if (IsDestructiveStep(step))
    {
        return EraseSequenceState::kNeedsHelper;
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
std::optional<PlanValidationFailure> ValidateSessionPlan(Steps&& steps)
{
    bool saw_session_step = false;
    EraseSequenceState erase_sequence = EraseSequenceState::kNeedsHelper;
    for (const PreparedStep& step : steps)
    {
        if (step.spec.id == CommandId::Ports)
        {
            continue;
        }
        if (step.spec.id == CommandId::Connect && saw_session_step)
        {
            return PlanValidationFailure{
                &step, Error{ErrorKind::kInvalidConfig,
                             "connect must be the first non-ports session step and may appear only once"}};
        }
        saw_session_step = true;

        if (step.spec.id == CommandId::Erase && erase_sequence != EraseSequenceState::kReady)
        {
            return PlanValidationFailure{&step,
                                         Error{ErrorKind::kInvalidConfig, std::string(kEraseSequenceRequirement)}};
        }
        erase_sequence = AdvanceEraseSequence(erase_sequence, step, true);
    }
    return std::nullopt;
}

struct SessionState
{
    EraseSequenceState erase_sequence = EraseSequenceState::kNeedsHelper;
};

int RunSteps(IBenchSession& session, IBenchFiles& files, const Reporter& reporter, std::span<const PreparedStep> steps,
             SessionState& state)
{
    BenchContext context{.session = session, .files = files, .options = reporter.options};
    int code = 0;
    for (const PreparedStep& step : steps)
    {
        CommandOutcome outcome;
        if (step.spec.id == CommandId::Erase && state.erase_sequence != EraseSequenceState::kReady)
        {
            outcome = FailedOutcome(render_step(step.spec),
                                    Error{ErrorKind::kInvalidConfig, std::string(kEraseSequenceRequirement)});
            if (const Result<double> battery = session.vbatt(); battery.has_value())
            {
                outcome.vbatt = *battery;
            }
        }
        else
        {
            outcome = run_step(context, step);
        }

        state.erase_sequence = AdvanceEraseSequence(state.erase_sequence, step, outcome.ok);

        reporter.Report(outcome);
        if (outcome.ok)
        {
            continue;
        }
        Ratchet(code, exit_code_for(outcome.error_kind.value_or(ErrorKind::kInternal)));
        if (!reporter.options.keep_going)
        {
            break;
        }
    }
    return code;
}

int RunPorts(IBenchEnvironment& environment, const Reporter& reporter)
{
    const Result<std::vector<std::string>> ports = environment.list_ports(reporter.options);
    if (!ports.has_value())
    {
        return reporter.Fail("ports", ports.error());
    }

    if (!reporter.options.json)
    {
        for (const std::string& port : *ports)
        {
            reporter.output << port << '\n';
        }
        return 0;
    }

    reporter.Report(CommandOutcome{
        .step = "ports", .note = "ports=" + (*ports | std::views::join_with(',') | std::ranges::to<std::string>())});
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
std::expected<std::vector<PreparedStep>, PreparationFailure> PrepareSteps(IBenchFiles& files,
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
std::string PlanLabel(std::span<const PreparedStep> steps)
{
    return steps.empty() ? std::string("setup") : render_step(steps.front().spec);
}

// The one session every step shares. Connecting is implicit unless the plan
// opens with an explicit `connect`.
Result<std::reference_wrapper<IBenchSession>> OpenSession(IBenchEnvironment& environment, const GlobalOptions& options,
                                                          std::span<const PreparedStep> steps)
{
    const bool connect_implicitly =
        !options.no_connect && !steps.empty() && steps.front().spec.id != CommandId::Connect;
    return environment.session(options, connect_implicitly);
}

int RunBatch(IBenchEnvironment& environment, IBenchFiles& files, const Reporter& reporter,
             std::span<const StepSpec> steps)
{
    const std::expected<std::vector<PreparedStep>, PreparationFailure> prepared = PrepareSteps(files, steps);
    if (!prepared.has_value())
    {
        return reporter.Fail(render_step(*prepared.error().step), prepared.error().error);
    }
    if (const std::optional<PlanValidationFailure> failure = ValidateSessionPlan(*prepared); failure.has_value())
    {
        return reporter.Fail(render_step(failure->step->spec), failure->error);
    }

    const Result<std::reference_wrapper<IBenchSession>> session = OpenSession(environment, reporter.options, *prepared);
    if (!session.has_value())
    {
        return reporter.Fail(PlanLabel(*prepared), session.error(), environment.last_setup_traffic());
    }
    SessionState state;
    return RunSteps(session->get(), files, reporter, *prepared, state);
}

struct ScriptPlan
{
    std::vector<std::vector<PreparedStep>> lines;
    // Non-zero once a line failed to parse or prepare, which forbids executing
    // any line at all.
    int code = 0;
};

// Reads the whole script and prepares every line before any of them runs.
ScriptPlan PrepareScript(IBenchFiles& files, const Reporter& reporter, std::istream& input)
{
    ScriptPlan plan;
    // Reports the failure; true means stop reading, false means keep going to
    // show the operator every bad line before refusing to run any of them.
    const auto line_failed = [&](std::string label, const Error& error)
    {
        Ratchet(plan.code, reporter.Fail(std::move(label), error));
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
            if (const Error error{ErrorKind::kInvalidConfig,
                                  std::format("script-line global option {} is not allowed; put it on the outer "
                                              "--script invocation",
                                              *forbidden)};
                line_failed(line_label, error))
            {
                return plan;
            }
            continue;
        }

        const std::vector<std::string_view> line_args(tokens.begin(), tokens.end());
        const Result<ParsedCommandLine> parsed = parse_command_line(line_args);
        if (!parsed.has_value())
        {
            if (line_failed(line_label, parsed.error()))
            {
                return plan;
            }
            continue;
        }

        std::expected<std::vector<PreparedStep>, PreparationFailure> prepared = PrepareSteps(files, parsed->steps);
        if (!prepared.has_value())
        {
            if (line_failed(std::format("{}: {}", line_label, render_step(*prepared.error().step)),
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
bool IsPortsLine(std::span<const PreparedStep> steps)
{
    return !steps.empty() && steps.front().spec.id == CommandId::Ports;
}

int RunScriptLines(IBenchEnvironment& environment, IBenchFiles& files, const Reporter& reporter,
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
        if (IsPortsLine(steps))
        {
            code = RunPorts(environment, reporter);
        }
        else
        {
            if (!session.has_value())
            {
                const Result<std::reference_wrapper<IBenchSession>> opened =
                    OpenSession(environment, reporter.options, steps);
                if (!opened.has_value())
                {
                    return reporter.Fail(PlanLabel(steps), opened.error(), environment.last_setup_traffic());
                }
                session = *opened;
            }
            code = RunSteps(session->get(), files, reporter, steps, state);
        }

        Ratchet(first_code, code);
        if (first_code != 0 && !reporter.options.keep_going)
        {
            return first_code;
        }
    }
    return first_code;
}

int RunScript(IBenchEnvironment& environment, IBenchFiles& files, const Reporter& reporter, std::istream& input)
{
    const ScriptPlan plan = PrepareScript(files, reporter, input);
    // A script is a single destructive plan even though each line is executed
    // as a batch: a malformed line 7 must not be discovered after line 1 has
    // already erased, so one bad line cancels the whole script.
    if (plan.code != 0)
    {
        return plan.code;
    }
    if (const std::optional<PlanValidationFailure> failure = ValidateSessionPlan(plan.lines | std::views::join);
        failure.has_value())
    {
        return reporter.Fail(render_step(failure->step->spec), failure->error);
    }
    return RunScriptLines(environment, files, reporter, plan.lines);
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
            return Reporter{options, output, diagnostics}.Fail("command line", parsed.error());
        }
        diagnostics << parsed.error().detail << '\n';
        return exit_code_for(parsed.error().kind);
    }

    const Reporter reporter{parsed->options, output, diagnostics};
    if (parsed->options.script_stdin)
    {
        return RunScript(environment, files, reporter, input);
    }
    if (parsed->steps.front().id == CommandId::Ports)
    {
        return RunPorts(environment, reporter);
    }
    return RunBatch(environment, files, reporter, parsed->steps);
}

} // namespace fastecu::bench
