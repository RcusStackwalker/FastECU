"""Real configured-graph fixtures; no C++ fixture compilation is requested."""

load("//tools/sonar:compile_commands.bzl", "sonar_compile_commands")

def _transition_impl(settings, _attr):
    return {"//command_line_option:copt": settings["//command_line_option:copt"] + ["-DFASTECU_TRANSITION=1"]}

_transition = transition(
    implementation = _transition_impl,
    inputs = ["//command_line_option:copt"],
    outputs = ["//command_line_option:copt"],
)

def _forward_impl(ctx):
    return [DefaultInfo(files = ctx.attr.binary[0][DefaultInfo].files)]

graph_forward = rule(
    implementation = _forward_impl,
    attrs = {
        "binary": attr.label(cfg = _transition),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)

def _outputs_impl(ctx):
    groups = [target[OutputGroupInfo] for target in ctx.attr.targets]
    fragments = depset(transitive = [group.sonar_commands for group in groups]).to_list()
    inputs = depset(transitive = [group.sonar_inputs for group in groups])
    outputs = []

    # Runfiles otherwise collapse the same label's different configurations.
    for index, file in enumerate(fragments + [file for file in inputs.to_list() if file.basename == "generated.h"]):
        output = ctx.actions.declare_file("graph/%s/%s" % (index, file.basename))
        ctx.actions.symlink(output = output, target_file = file)
        outputs.append(output)
    stamp = ctx.actions.declare_file("graph/inputs.checked")
    ctx.actions.run_shell(inputs = inputs, outputs = [stamp], arguments = [stamp.path], command = "touch \"$1\"")
    return [DefaultInfo(files = depset(outputs + [stamp]))]

graph_outputs = rule(
    implementation = _outputs_impl,
    attrs = {"targets": attr.label_list(aspects = [sonar_compile_commands])},
)
