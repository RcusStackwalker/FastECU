"""Keep Python tooling and test fixtures in the intended Sonar analysis scope."""

import ast
import unittest
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).parents[2]
OWNERS = (
    "scripts",
    "tools/clang-tidy/scripts",
    "tools/clang-tidy/plugin",
    "tools/sonar",
    "packaging",
)
FIXTURES = ("tools/clang-tidy/plugin/test", "tools/clang-tidy/plugin/smoke")


def properties():
    text = (ROOT / "sonar-project.properties").read_text().replace("\\\n", "")
    return {
        key.strip(): tuple(value.strip() for value in values.split(",") if value.strip())
        for line in text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
        for key, separator, values in [line.partition("=")]
        if separator
    }


def under(path, directories):
    return any(path == directory or path.startswith(directory + "/") for directory in directories)


def matches(path, patterns):
    return any(PurePosixPath(path).full_match(pattern, case_sensitive=True) for pattern in patterns)


def test_path(path):
    return "tests" in PurePosixPath(path).parts or under(path, FIXTURES)


class SonarScopeTest(unittest.TestCase):
    def test_python_and_fixtures_have_the_right_analysis_scope(self):
        config = properties()
        for owner in OWNERS:
            for file in sorted((ROOT / owner).rglob("*")):
                if not file.is_file() or "__pycache__" in file.parts:
                    continue
                path = file.relative_to(ROOT).as_posix()
                if file.suffix != ".py" and not under(path, FIXTURES):
                    continue
                with self.subTest(path=path):
                    is_test = test_path(path)
                    source_inclusions = config.get("sonar.inclusions", ())
                    test_inclusions = config.get("sonar.test.inclusions", ())
                    source = (
                        under(path, config["sonar.sources"])
                        and (not source_inclusions or matches(path, source_inclusions))
                        and not matches(path, config.get("sonar.exclusions", ()))
                        # Sonar automatically excludes test inclusion patterns from sources.
                        and not matches(path, test_inclusions)
                    )
                    test = (
                        under(path, config["sonar.tests"])
                        and (not test_inclusions or matches(path, test_inclusions))
                        and not matches(path, config.get("sonar.test.exclusions", ()))
                    )
                    self.assertEqual(
                        (source, test),
                        (not is_test, is_test),
                        "expected exactly one scope matching the file's purpose",
                    )

    def test_python_test_entry_points_are_in_test_directories(self):
        for package in (
            "",
            "tools/clang-tidy/scripts",
            "tools/clang-tidy/plugin",
            "tools/sonar",
            "packaging",
        ):
            build = ROOT / package / "BUILD.bazel"
            for node in ast.walk(ast.parse(build.read_text())):
                if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
                    continue
                if node.func.id != "py_test":
                    continue
                attributes = {keyword.arg: keyword.value for keyword in node.keywords}
                name = ast.literal_eval(attributes["name"])
                main = (
                    ast.literal_eval(attributes["main"]) if "main" in attributes else name + ".py"
                )
                path = (PurePosixPath(package) / main).as_posix()
                with self.subTest(target=f"//{package}:{name}"):
                    self.assertTrue(
                        test_path(path), f"test entry point outside a test directory: {path}"
                    )
                    self.assertTrue((ROOT / path).is_file(), f"undeclared test input: {path}")

    def test_unittest_suites_are_in_test_directories(self):
        for owner in OWNERS:
            for file in sorted((ROOT / owner).rglob("*.py")):
                if "__pycache__" in file.parts:
                    continue
                tree = ast.parse(file.read_text())
                uses_unittest = any(
                    isinstance(node, ast.Import)
                    and any(alias.name == "unittest" for alias in node.names)
                    or isinstance(node, ast.ImportFrom)
                    and node.module == "unittest"
                    for node in ast.walk(tree)
                )
                if uses_unittest:
                    path = file.relative_to(ROOT).as_posix()
                    with self.subTest(path=path):
                        self.assertTrue(
                            test_path(path), "unittest code belongs in a test directory"
                        )


if __name__ == "__main__":
    unittest.main()
