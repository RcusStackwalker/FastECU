"""Run the Bazel-pinned sonar-scanner from the workspace root."""

import os
import sys
from collections.abc import Sequence
from pathlib import Path

from python.runfiles import runfiles

SCANNER = "sonar_scanner_cli/bin/sonar-scanner"


def scan_command(scanner: Path, args: Sequence[str]) -> list[str]:
    # The scanner archive bundles its own JRE, so Sonar must not provision one.
    return [str(scanner), "-Dsonar.scanner.skipJreProvisioning=true", *args]


def main(argv: Sequence[str]) -> int:
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not workspace:
        print("BUILD_WORKSPACE_DIRECTORY is not set; run via `bazel run`.", file=sys.stderr)
        return 2
    scanner = runfiles.Create().Rlocation(SCANNER)
    if not scanner or not Path(scanner).is_file():
        print(f"{SCANNER} is not in the runfiles; run via `bazel run`.", file=sys.stderr)
        return 2
    os.chdir(workspace)
    command = scan_command(Path(scanner), argv)
    os.execv(command[0], command)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
