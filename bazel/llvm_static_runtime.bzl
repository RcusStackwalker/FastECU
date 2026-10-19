"""Match the Windows LLVM development archive's static release CRT."""

def _static_runtime_impl(settings, _attr):
    features = [f for f in settings["//command_line_option:features"] if f not in ["dynamic_link_msvcrt", "-static_link_msvcrt"]]
    return {"//command_line_option:features": features + ["static_link_msvcrt", "-dynamic_link_msvcrt"]}

_static_runtime = transition(
    implementation = _static_runtime_impl,
    inputs = ["//command_line_option:features"],
    outputs = ["//command_line_option:features"],
)

def _llvm_static_binary_impl(ctx):
    binary = ctx.attr.binary[0][DefaultInfo].files_to_run.executable
    output = ctx.actions.declare_file(ctx.label.name + ".exe")
    ctx.actions.symlink(output = output, target_file = binary, is_executable = True)
    return [DefaultInfo(executable = output, runfiles = ctx.runfiles(files = [binary]))]

llvm_static_binary = rule(
    implementation = _llvm_static_binary_impl,
    executable = True,
    attrs = {
        "binary": attr.label(cfg = _static_runtime, executable = True, mandatory = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)
