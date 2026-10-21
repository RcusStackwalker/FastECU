"""The clang-tidy lint aspect (docs/superpowers/specs/2026-10-21-clang-tidy-aspect-spike.md)."""

load("@aspect_rules_lint//lint:clang_tidy.bzl", "lint_clang_tidy_aspect")
load("@local_llvm//:defs.bzl", "CLANG_TIDY_PLATFORM_ARGS")
load("//bazel:clang_tidy_config_chains.bzl", "CONFIG_CHAINS", "CONFIG_LABELS")

clang_tidy = lint_clang_tidy_aspect(
    binary = Label("//tools/clang-tidy/aspect:clang_tidy"),
    # CONFIG_LABELS and CONFIG_CHAINS come from scripts/clang_tidy_configs.py: a source gets
    # only the .clang-tidy files that govern it.
    configs = [Label(label) for label in CONFIG_LABELS],
    config_chains = CONFIG_CHAINS,
    plugins = [Label("//tools/clang-tidy/aspect:plugins")],
    deps = [Label("@local_llvm//:clang_resource_headers")],
    rule_kinds = ["cc_binary", "cc_library", "cc_test"],
    args = CLANG_TIDY_PLATFORM_ARGS,
)
