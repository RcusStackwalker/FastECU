#!/usr/bin/env python3
"""Check that Bazel source manifests still match qmake project files."""

from __future__ import annotations

import ast
import posixpath
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _tokens(value: str) -> list[str]:
    value = re.sub(r"#.*", "", value).replace("\\", " ")
    return [token.strip().strip('"') for token in re.split(r"\s+", value.strip()) if token]


def parse_qmake(path: str) -> dict[str, dict[str, list[str]]]:
    variables = ("SOURCES", "HEADERS", "FORMS", "RESOURCES", "REPC_REPLICA")
    result = {
        "common": {key: [] for key in variables},
        "unix": {key: [] for key in variables},
        "win32": {key: [] for key in variables},
        "macx": {key: [] for key in variables},
        "linux": {key: [] for key in variables},
    }
    condition = "common"
    stack: list[str] = []
    statement = ""

    for raw_line in (ROOT / path).read_text().splitlines():
        line = raw_line.split("#", 1)[0].rstrip()
        if not line:
            continue
        stripped = line.strip()
        if stripped.endswith("{") and not statement:
            qmake_condition = stripped[:-1].strip()
            if qmake_condition.startswith("unix:!macx"):
                condition = "linux"
            elif qmake_condition.startswith("unix"):
                condition = "unix"
            elif qmake_condition.startswith("win32"):
                condition = "win32"
            elif qmake_condition.startswith("macx"):
                condition = "macx"
            else:
                condition = "common"
            stack.append(condition)
            continue
        if stripped == "}" and not statement:
            if stack:
                stack.pop()
            condition = stack[-1] if stack else "common"
            continue

        statement += " " + stripped
        if statement.rstrip().endswith("\\"):
            statement = statement.rstrip()[:-1]
            continue

        stmt = statement.strip()
        statement = ""
        inline = re.match(
            r"(unix:!macx|unix|win32|macx):\s*(SOURCES|HEADERS|FORMS|RESOURCES|REPC_REPLICA)\s*(?:\+=|=)\s*(.*)",
            stmt,
        )
        if inline:
            dest = {
                "unix:!macx": "linux",
                "unix": "unix",
                "win32": "win32",
                "macx": "macx",
            }[inline.group(1)]
            variable = inline.group(2)
            value = inline.group(3)
        else:
            match = re.match(
                r"(SOURCES|HEADERS|FORMS|RESOURCES|REPC_REPLICA)\s*(?:\+=|=)\s*(.*)",
                stmt,
            )
            if not match:
                continue
            dest = condition
            variable = match.group(1)
            value = match.group(2)

        result[dest][variable].extend(_tokens(value))

    return result


def normalize(items: list[str], base: str = "") -> list[str]:
    normalized: list[str] = []
    for token in items:
        token = token.replace("\\", "/").replace("$$PWD", base.rstrip("/"))
        if not token or token.startswith("$$") or token.startswith("/"):
            continue
        if base and not token.startswith(base + "/") and not token.startswith("../"):
            token = f"{base}/{token}"
        normalized.append(posixpath.normpath(token))
    return sorted(dict.fromkeys(normalized))


def load_bazel_lists() -> dict[str, list[str]]:
    module = ast.parse((ROOT / "bazel/fastecu_sources.bzl").read_text())
    values: dict[str, list[str]] = {}
    for node in module.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            if isinstance(target, ast.Name) and isinstance(node.value, ast.List):
                values[target.id] = [elt.value for elt in node.value.elts if isinstance(elt, ast.Constant)]
    return values


def q_object_headers(headers: list[str]) -> list[str]:
    def has_q_object(path: Path) -> bool:
        in_block_comment = False
        for raw_line in path.read_text(errors="ignore").splitlines():
            line = raw_line
            if in_block_comment:
                if "*/" in line:
                    line = line.split("*/", 1)[1]
                    in_block_comment = False
                else:
                    continue
            while "/*" in line:
                before, after = line.split("/*", 1)
                if "*/" in after:
                    line = before + after.split("*/", 1)[1]
                else:
                    line = before
                    in_block_comment = True
                    break
            code = line.split("//", 1)[0]
            if re.search(r"\bQ_OBJECT\b", code):
                return True
        return False

    return sorted(header for header in headers if (ROOT / header).exists() and has_q_object(ROOT / header))


def expect_equal(name: str, expected: list[str], actual: list[str], errors: list[str]) -> None:
    expected_set = set(expected)
    actual_set = set(actual)
    missing = sorted(expected_set - actual_set)
    extra = sorted(actual_set - expected_set)
    if missing or extra:
        errors.append(f"{name} differs")
        if missing:
            errors.append("  missing in Bazel: " + ", ".join(missing))
        if extra:
            errors.append("  extra in Bazel: " + ", ".join(extra))


def main() -> int:
    bazel = load_bazel_lists()
    errors: list[str] = []

    app = parse_qmake("FastECU.pro")
    protocol = parse_qmake("protocol/protocol.pri")
    app["common"]["SOURCES"] += normalize(protocol["common"]["SOURCES"], "protocol")
    app["common"]["HEADERS"] += normalize(protocol["common"]["HEADERS"], "protocol")

    app_common_sources = [src for src in normalize(app["common"]["SOURCES"]) if src != "main.cpp"]
    app_common_headers = normalize(app["common"]["HEADERS"])
    app_unix_headers = normalize(app["unix"]["HEADERS"])
    app_win_headers = normalize(app["win32"]["HEADERS"])
    app_moc_headers = q_object_headers(app_common_headers + app_unix_headers + app_win_headers)
    app_common_moc_headers = [
        header for header in app_moc_headers if header not in app_unix_headers and header not in app_win_headers
    ]

    expect_equal("APP_MAIN_SRCS", ["main.cpp"], bazel["APP_MAIN_SRCS"], errors)
    expect_equal("APP_COMMON_SRCS", app_common_sources, bazel["APP_COMMON_SRCS"], errors)
    expect_equal(
        "APP_COMMON_HDRS",
        [header for header in app_common_headers if header not in app_common_moc_headers],
        bazel["APP_COMMON_HDRS"],
        errors,
    )
    expect_equal("APP_MOC_HDRS", app_moc_headers, bazel["APP_MOC_HDRS"], errors)
    expect_equal("APP_FORMS", normalize(app["common"]["FORMS"]), bazel["APP_FORMS"], errors)
    expect_equal("APP_RESOURCES", normalize(app["common"]["RESOURCES"]), bazel["APP_RESOURCES"], errors)
    expect_equal("APP_REPS", normalize(app["common"]["REPC_REPLICA"]), bazel["APP_REPS"], errors)
    expect_equal("APP_UNIX_SRCS", normalize(app["unix"]["SOURCES"]), bazel["APP_UNIX_SRCS"], errors)
    expect_equal("APP_UNIX_HDRS", app_unix_headers, bazel["APP_UNIX_HDRS"], errors)
    expect_equal("APP_WIN_SRCS", normalize(app["win32"]["SOURCES"]), bazel["APP_WIN_SRCS"], errors)
    expect_equal("APP_WIN_HDRS", app_win_headers, bazel["APP_WIN_HDRS"], errors)

    projects = {
        "MUT_DMA_TESTS": ("tests/tests.pro", "tests", True),
        "SERIAL_BACKEND_TESTS": ("tests/serial_backend_tests.pro", "tests", False),
        "SERIAL_CRASH_TESTS": ("tests/serial_crash_tests.pro", "tests", False),
        "MUT_DMA_INTEGRATION_TESTS": ("tests/mut_dma_integration_tests.pro", "tests", False),
        "TST_FORCE_ASSERTS": ("tests/force_asserts/force_asserts.pro", "tests/force_asserts", False),
    }
    for prefix, (path, base, includes_protocol) in projects.items():
        parsed = parse_qmake(path)
        for condition in ("common", "unix", "win32"):
            sources = normalize(parsed[condition]["SOURCES"], base)
            headers = normalize(parsed[condition]["HEADERS"], base)
            if includes_protocol and condition == "common":
                sources += normalize(protocol["common"]["SOURCES"], "protocol")
                headers += normalize(protocol["common"]["HEADERS"], "protocol")
                sources = sorted(dict.fromkeys(sources))
                headers = sorted(dict.fromkeys(headers))
            expect_equal(f"{prefix}_{condition.upper()}_SRCS", sources, bazel[f"{prefix}_{condition.upper()}_SRCS"], errors)
            expect_equal(f"{prefix}_{condition.upper()}_HDRS", headers, bazel[f"{prefix}_{condition.upper()}_HDRS"], errors)
            expect_equal(
                f"{prefix}_{condition.upper()}_REPS",
                normalize(parsed[condition]["REPC_REPLICA"], base),
                bazel[f"{prefix}_{condition.upper()}_REPS"],
                errors,
            )

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Bazel source manifest is synchronized with qmake project files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
