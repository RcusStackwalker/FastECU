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


def main() -> None:
    if len(sys.argv) != 1:
        sys.exit("refresh_sonar does not accept arguments")

    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not workspace:
        sys.exit("refresh_sonar must be run via `bazel run`")

    r = runfiles.Create()
    refresh = r.Rlocation(REFRESH_RUNFILE)
    if not refresh or not os.path.isabs(refresh) or not os.path.isfile(refresh):
        sys.exit("could not locate :refresh binary in this target's runfiles")

    subprocess.run([refresh], check=True)

    db_path = pathlib.Path(workspace) / "compile_commands.json"
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
