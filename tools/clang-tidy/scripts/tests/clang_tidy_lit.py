#!/usr/bin/env python3
"""Supply the Bazel-built analysis host and LLVM tools to the lit suite."""

import argparse
import os
import shutil
import sys
import tempfile
from pathlib import Path

import clang_tidy_toolchain as toolchain
from lit.main import main


def run() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", required=True)
    parser.add_argument("--filecheck", required=True)
    parser.add_argument("--gtest-header", required=True)
    parser.add_argument("--gmock-header", required=True)
    args, lit_args = parser.parse_known_args()
    host = toolchain.runfiles_analysis_host(os.environ)
    suite = Path(__file__).parents[2] / "plugin" / "test"
    with tempfile.TemporaryDirectory(prefix="fastecu-lit-") as directory:
        tools = Path(directory) / "bin"
        tools.mkdir()
        suffix = ".exe" if os.name == "nt" else ""
        for name, source in [("clang-tidy", host.executable), ("FileCheck", Path(args.filecheck))]:
            target = tools / (name + suffix)
            if os.name == "nt":
                try:
                    shutil.copy2(source, target)
                except OSError:
                    print(
                        f"[DEBUG-lit-tools] source={source} resolved={source.resolve()} "
                        f"metadata={source.lstat()} symlink={source.is_symlink()}",
                        file=sys.stderr,
                    )
                    raise
            else:
                target.symlink_to(source.resolve())
        sys.argv = [sys.argv[0], "-v", str(suite), *lit_args]
        main(
            builtin_params={
                "exec_root": directory,
                "tools": str(tools),
                "helper": str(Path(args.helper).resolve()),
                "llvm_root": str(host.llvm_root),
                "load": list(host.arguments),
                "workspace": str(suite.parents[3]),
                "gtest": str(Path(args.gtest_header).resolve().parents[1]),
                "gmock": str(Path(args.gmock_header).resolve().parents[1]),
            }
        )


if __name__ == "__main__":
    run()
