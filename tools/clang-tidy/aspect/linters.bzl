"""The clang-tidy lint aspect (docs/superpowers/specs/2026-10-21-clang-tidy-aspect-spike.md)."""

load("@aspect_rules_lint//lint:clang_tidy.bzl", "lint_clang_tidy_aspect")
load("@local_llvm//:defs.bzl", "CLANG_TIDY_PLATFORM_ARGS")

clang_tidy = lint_clang_tidy_aspect(
    binary = Label("//tools/clang-tidy/aspect:clang_tidy"),
    # The exceptions policy is read by the fastecu checks themselves, relative to the
    # working directory, so it has to be an input like the configuration.
    configs = [
        Label("//:.clang-tidy"),
        Label("//:.clang-tidy-exceptions.yaml"),
    ],
    plugins = [Label("//tools/clang-tidy/plugin:fastecu_tidy")],
    rule_kinds = ["cc_binary", "cc_library", "cc_test"],
    args = CLANG_TIDY_PLATFORM_ARGS,
)
