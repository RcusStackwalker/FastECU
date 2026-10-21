"""An executable graph regression for lint outputs across a platform transition."""

load("//tools/clang-tidy/aspect:linters.bzl", "clang_tidy")

def _transition_impl(settings, attr):
    flags = ["/DFASTECU_EXPECT_POINTER_BYTES=4"] if attr.windows_x86 else ["-DFASTECU_LINT_TRANSITION=1"]
    return {
        "//command_line_option:copt": settings["//command_line_option:copt"] + flags,
        "//command_line_option:platforms": ["//bazel/platforms:windows_x86"] if attr.windows_x86 else settings["//command_line_option:platforms"],
    }

_transition = transition(
    implementation = _transition_impl,
    inputs = ["//command_line_option:copt", "//command_line_option:platforms"],
    outputs = ["//command_line_option:copt", "//command_line_option:platforms"],
)

def _forward_impl(ctx):
    return [DefaultInfo(files = ctx.attr.binary[0][DefaultInfo].files)]

forward = rule(
    implementation = _forward_impl,
    attrs = {
        "binary": attr.label(cfg = _transition),
        "windows_x86": attr.bool(),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)

def _outputs_impl(ctx):
    target = ctx.attr.binary
    if OutputGroupInfo not in target:
        fail("missing transitioned lint outputs")
    info = target[OutputGroupInfo]
    human = getattr(info, "rules_lint_human", depset())
    if not human:
        fail("missing transitioned lint outputs")
    return [DefaultInfo(files = depset(transitive = [human, info.rules_lint_fixes]))]

lint_outputs = rule(
    implementation = _outputs_impl,
    attrs = {"binary": attr.label(aspects = [clang_tidy])},
)
