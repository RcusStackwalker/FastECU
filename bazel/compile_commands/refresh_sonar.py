"""`bazel run //bazel/compile_commands:refresh_sonar`

Generate compile_commands.json for SonarCloud from the Bazel graph.

Runs Hedron's :refresh to emit the database into the workspace root, then
rewrites Bazel's cc_wrapper.sh compiler (argv[0] of each entry) to plain clang.
Sonar's C/C++ analyzer can't introspect that wrapper -- it logs "Ignore unknown
compiler" and skips every compilation unit ("0 files analyzed") -- but the
remaining arguments are already a valid clang invocation (argv[1] is -xc++).

argv[1] is the runfiles path of the :refresh binary (passed by the BUILD rule
via $(rlocationpath)); any further args are forwarded to refresh, which passes
them through to the bazel commands it runs for extraction.
"""

import json
import os
import pathlib
import subprocess
import sys

from python.runfiles import runfiles


def main() -> None:
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not workspace:
        sys.exit("refresh_sonar must be run via `bazel run`")

    r = runfiles.Create()
    refresh = r.Rlocation(sys.argv[1])
    if not refresh or not os.path.exists(refresh):
        sys.exit(f"could not locate :refresh binary at {sys.argv[1]!r}")

    subprocess.run([refresh, *sys.argv[2:]], check=True)

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
