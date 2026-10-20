"""LLVM-style check tests, using real FastECU and GoogleTest headers."""

import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

import lit.formats

config = globals()["config"]
lit_config = globals()["lit_config"]

config.name = "FastECU clang-tidy"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".cpp", ".test"]
config.excludes = ["Inputs"]
config.test_source_root = str(Path(__file__).parent)
config.test_exec_root = lit_config.params["exec_root"]
config.environment["PATH"] = lit_config.params["tools"] + os.pathsep + os.environ["PATH"]
config.environment["CLANG_NO_DEFAULT_CONFIG"] = "1"
# The upstream helper invokes diff as a subprocess, including on Windows.
if os.name == "nt" and (git := shutil.which("git")):
    config.environment["PATH"] += os.pathsep + str(Path(git).parents[1] / "usr/bin")

# check_clang_tidy.py intentionally uses -nostdinc++. Supply the selected
# compiler's real search paths to retain the integration coverage of these tests.
compiler = (
    Path(lit_config.params["llvm_root"]) / "bin" / ("clang++.exe" if os.name == "nt" else "clang++")
)
platform_flags = []
if sys.platform == "darwin":
    sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
    platform_flags = ["-isysroot", sdk]
probe = subprocess.run(
    [str(compiler), *platform_flags, "-E", "-x", "c++", "-", "-v"],
    input="",
    text=True,
    capture_output=True,
    check=True,
)
search = probe.stderr.split("#include <...> search starts here:", 1)[1].split(
    "End of search list.", 1
)[0]
flags = [
    *platform_flags,
    "-I" + lit_config.params["workspace"],
    "-isystem",
    lit_config.params["gtest"],
    "-isystem",
    lit_config.params["gmock"],
]
for path in search.splitlines():
    path = path.strip()
    if path and not path.endswith("(framework directory)"):
        flags += ["-isystem", path]
config.substitutions += [
    ("%check_clang_tidy", shlex.join([sys.executable, lit_config.params["helper"]])),
    ("%clang_tidy", shlex.quote(str(Path(lit_config.params["tools"]) / "clang-tidy"))),
    ("%load", shlex.join(lit_config.params["load"])),
    ("%cxx_flags", shlex.join(flags)),
    (
        "%check_config",
        shlex.quote(
            "--config="
            + json.dumps(
                {
                    "CheckOptions": {
                        "fastecu.ExceptionsFile": str(
                            Path(__file__).parent / "Inputs/empty-exceptions.yaml"
                        )
                    }
                }
            )
        ),
    ),
    ("%workspace", shlex.quote(lit_config.params["workspace"])),
]
