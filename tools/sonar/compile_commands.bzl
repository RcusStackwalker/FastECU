"""Export real configured compiler actions and materialize only their inputs."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

_EDGES = ["deps", "implementation_deps", "binary", "actual"]

def _dependencies(ctx):
    targets = []
    for edge in _EDGES:
        value = getattr(ctx.rule.attr, edge, [])
        if type(value) == "Target":
            targets.append(value)
        elif type(value) == "list":
            targets.extend(value)
        elif type(value) == "dict":
            for split in value.values():
                targets.extend(split if type(split) == "list" else [split])
    return [target[OutputGroupInfo] for target in targets if OutputGroupInfo in target]

def _sonar_impl(target, ctx):
    if target.label.workspace_name:
        return [OutputGroupInfo(sonar_commands = depset(), sonar_inputs = depset())]
    dependencies = _dependencies(ctx)
    fragments = []
    inputs = []
    commands = []

    # CppCompile inputs omit headers deferred to include discovery. The scanner
    # never runs that compile, so request the declared compilation header closure.
    headers = target[CcInfo].compilation_context.headers if CcInfo in target else depset()
    for action in target.actions:
        if action.mnemonic != "CppCompile":
            continue
        argv = action.argv
        if not argv or "-c" not in argv or argv.index("-c") + 1 >= len(argv):
            fail("unsupported CppCompile command for %s" % target.label)
        if any([argument.startswith("@") for argument in argv]):
            fail("unexpanded compiler response file for %s" % target.label)
        command_inputs = depset(transitive = [action.inputs, headers])
        commands.append({
            "file": argv[argv.index("-c") + 1],
            "arguments": argv,
            "outputs": sorted([file.path for file in action.outputs.to_list()]),
        })
        inputs.append(command_inputs)
    if commands:
        fragment = ctx.actions.declare_file(ctx.label.name + ".sonar.json")
        ctx.actions.write(fragment, json.encode({
            "version": 2,
            "owner": str(target.label),
            "configuration": ctx.bin_dir.path,
            "commands": commands,
        }))
        fragments.append(fragment)
    return [OutputGroupInfo(
        sonar_commands = depset(fragments, transitive = [getattr(dep, "sonar_commands", depset()) for dep in dependencies]),
        sonar_inputs = depset(transitive = inputs + [getattr(dep, "sonar_inputs", depset()) for dep in dependencies]),
    )]

sonar_compile_commands = aspect(
    implementation = _sonar_impl,
    attr_aspects = _EDGES,
)
