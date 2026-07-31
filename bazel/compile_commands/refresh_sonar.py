"""`bazel run //bazel/compile_commands:refresh_sonar`

Generate compile_commands.json for SonarCloud from the Bazel graph.

Runs Hedron's :refresh to emit the database into the workspace root, then
rewrites Bazel's cc_wrapper.sh compiler (argv[0] of each entry) to plain clang.
Sonar's C/C++ analyzer can't introspect that wrapper -- it logs "Ignore unknown
compiler" and skips every compilation unit ("0 files analyzed") -- but the
remaining arguments are already a valid clang invocation (argv[1] is -xc++).

The :refresh runfile is resolved using its fixed main-repository path. This
wrapper deliberately accepts no command-line arguments because :refresh passes
arguments through to Bazel commands.
"""

import json
import os
import pathlib
import subprocess
import sys

from python.runfiles import runfiles

REFRESH_RUNFILE = "_main/bazel/compile_commands/refresh"


def validated_workspace_root(value: str) -> pathlib.Path:
    root = pathlib.Path(value)
    if not root.is_absolute():
        raise ValueError(f"BUILD_WORKSPACE_DIRECTORY is not absolute: {value}")
    root = root.resolve()
    if not (root / "MODULE.bazel").is_file():
        raise ValueError(f"BUILD_WORKSPACE_DIRECTORY is not a Bazel workspace: {root}")
    return root


def main() -> None:
    if len(sys.argv) != 1:
        sys.exit("refresh_sonar does not accept arguments")

    workspace_value = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not workspace_value:
        sys.exit("refresh_sonar must be run via `bazel run`")
    try:
        workspace = validated_workspace_root(workspace_value)
    except ValueError as error:
        sys.exit(str(error))

    r = runfiles.Create()
    refresh = r.Rlocation(REFRESH_RUNFILE)
    if not refresh or not os.path.isabs(refresh) or not os.path.isfile(refresh):
        sys.exit("could not locate :refresh binary in this target's runfiles")

    subprocess.run([refresh], check=True)

    db_path = (workspace / "compile_commands.json").resolve()
    if not db_path.is_relative_to(workspace):
        sys.exit(
            f"internal error: compile_commands.json path escaped the Bazel workspace: {db_path}"
        )
    db = json.loads(db_path.read_text())
    rewritten = 0
    for entry in db:
        args = entry.get("arguments")
        if args and args[0].endswith("cc_wrapper.sh"):
            args[0] = "clang"
            rewritten += 1
    db_path.write_text(json.dumps(db))
    print(f"rewrote compiler in {rewritten}/{len(db)} entries")


if __name__ == "__main__":
    main()
