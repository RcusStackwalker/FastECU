"""Qualify the actual host/extension pair without application dependencies."""

import argparse
import subprocess
import tempfile
from pathlib import Path

import yaml


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang-tidy", required=True, type=Path)
    parser.add_argument("--plugin", type=Path)
    options = parser.parse_args()
    source = Path(__file__).with_name("smoke.cpp").resolve()
    with tempfile.TemporaryDirectory(prefix="fastecu-tidy-smoke-") as directory:
        exported = Path(directory) / "diagnostics.yaml"
        command = [
            str(options.clang_tidy.resolve()),
            "--config={}",
            "--checks=-*,fastecu-template-typename",
            "--warnings-as-errors=*",
            f"--export-fixes={exported}",
        ]
        if options.plugin:
            command.append(f"--load={options.plugin.resolve()}")
        command.extend([str(source), "--", "-std=c++20"])
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        print(result.stdout, end="")
        print(result.stderr, end="")
        report = yaml.safe_load(exported.read_text()) if exported.exists() else {}
        diagnostics = (report or {}).get("Diagnostics", [])
        expected_offset = source.read_bytes().index(b"class Value")
        if len(diagnostics) != 1:
            raise SystemExit(f"expected one diagnostic, received {len(diagnostics)}")
        diagnostic = diagnostics[0]
        message = diagnostic["DiagnosticMessage"]
        replacements = message["Replacements"]
        if (
            result.returncode != 1
            or diagnostic["DiagnosticName"] != "fastecu-template-typename"
            or diagnostic["Level"] != "Error"
            or Path(message["FilePath"]).resolve() != source
            or message["FileOffset"] != expected_offset
            or len(replacements) != 1
            or replacements[0]["ReplacementText"] != "typename"
            or replacements[0]["Offset"] != expected_offset
            or replacements[0]["Length"] != len("class")
        ):
            raise SystemExit(f"unexpected diagnostic or replacement: {diagnostic}")
        print("PASS: host emitted the registered diagnostic and typename fix-it")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
