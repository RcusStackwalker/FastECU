#!/usr/bin/env python3
"""Activate inline checks while retaining LLVM's parallel runner."""

from __future__ import annotations

import argparse
import asyncio
import importlib.machinery
import importlib.util
import inspect
import sys
from collections.abc import Sequence
from pathlib import Path


def run(upstream: Path, arguments: Sequence[str]) -> None:
    loader = importlib.machinery.SourceFileLoader("fastecu_upstream_clang_tidy", str(upstream))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    if spec is None:
        raise RuntimeError("incompatible run-clang-tidy: unable to load LLVM runner")
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    invocation = getattr(module, "get_tidy_invocation", None)
    main = getattr(module, "main", None)
    if not callable(invocation) or not inspect.iscoroutinefunction(main):
        raise RuntimeError(
            "incompatible run-clang-tidy: expected LLVM 23 invocation builder and async main"
        )

    def custom_invocation(*args: object, **kwargs: object) -> list[str]:
        command = invocation(*args, **kwargs)
        if not isinstance(command, list) or not command:
            raise RuntimeError(
                "incompatible run-clang-tidy: invocation builder returned no command"
            )
        return [command[0], "--experimental-custom-checks", *command[1:]]

    module.get_tidy_invocation = custom_invocation
    sys.argv = [str(upstream), *arguments]
    asyncio.run(main())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", required=True, type=Path)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    try:
        run(args.upstream, arguments)
    except (OSError, RuntimeError) as error:
        print(f"clang-tidy adapter: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
