"""Platform transition + forwarding rule for building a single target under
//bazel/platforms:windows_x86 while the rest of the build graph stays on the
host platform, in one Bazel invocation.

Without this, consuming an x86-only artifact from a host-platform target
(e.g. a test that spawns it as a subprocess) would require a second, separate
`bazel build --platforms=//bazel/platforms:windows_x86 ...` invocation and
manual path-wiring between the two -- exactly the shape
scripts/compile-x86-bridge-artifacts.ps1 plus CI --test_env plumbing had.
"""

def _x86_windows_transition_impl(_settings, _attr):
    return {"//command_line_option:platforms": ["//bazel/platforms:windows_x86"]}

_x86_windows_transition = transition(
    implementation = _x86_windows_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
)

def _x86_windows_binary_impl(ctx):
    src = ctx.attr.binary[0][DefaultInfo].files.to_list()[0]
    out = ctx.actions.declare_file(ctx.label.name + ctx.attr.extension)
    ctx.actions.symlink(output = out, target_file = src)
    return [DefaultInfo(
        files = depset([out]),
        runfiles = ctx.runfiles([out]),
    )]

x86_windows_binary = rule(
    implementation = _x86_windows_binary_impl,
    attrs = {
        "binary": attr.label(cfg = _x86_windows_transition, mandatory = True),
        "extension": attr.string(default = ""),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)
