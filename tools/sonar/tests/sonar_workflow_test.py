"""Protect the Sonar workflow's single-job ordering and retry boundaries."""

import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).parents[3]


class SonarWorkflowTest(unittest.TestCase):
    def setUp(self):
        workflow = yaml.safe_load((ROOT / ".github/workflows/sonar.yml").read_text())
        self.workflow = workflow
        self.jobs = workflow["jobs"]
        self.permissions = workflow["permissions"]
        self.steps = self.jobs["sonarcloud"]["steps"]

    def index(self, identifier):
        return next(i for i, step in enumerate(self.steps) if step.get("id") == identifier)

    def named(self, prefix):
        return next(
            i for i, step in enumerate(self.steps) if step.get("name", "").startswith(prefix)
        )

    def test_one_job_covers_converts_prepares_and_scans(self):
        self.assertEqual(list(self.jobs), ["sonarcloud"])
        self.assertNotIn("needs", self.jobs["sonarcloud"])
        order = [
            self.index("coverage"),
            self.index("coverage_report"),
            self.named("Prepare configured scanner inputs"),
            self.index("sonarcloud"),
            self.index("sonarcloud_retry"),
            self.named("Check SonarCloud Quality Gate"),
        ]
        self.assertEqual(order, sorted(order))

    def test_coverage_is_native_and_cached(self):
        self.assertIn(
            "bazel coverage --config=coverage //...", self.steps[self.index("coverage")]["run"]
        )
        for step in self.steps:
            run = step.get("run", "")
            # Spelled in halves so a repo-wide grep for the retired names stays empty.
            retired = (
                "coverage-" + "local.sh",
                "nocache_" + "test_results",
                "coverage_" + "artifact",
            )
            for forbidden in retired:
                self.assertNotIn(forbidden, run)
            if step.get("uses", "").startswith("actions/cache"):
                self.assertNotIn("github.run_id", step["with"]["key"])

    def test_scanner_reads_generic_coverage(self):
        properties = (ROOT / "sonar-project.properties").read_text()
        self.assertIn("sonar.coverageReportPaths=coverage/sonar-coverage.xml", properties)
        self.assertNotIn("llvm-cov.reportPath", properties)

    def test_it_keeps_the_caller_interface(self):
        trigger = self.workflow.get("on", self.workflow.get(True))
        self.assertEqual(list(trigger), ["workflow_call"])
        self.assertEqual(sorted(trigger["workflow_call"]["inputs"]), ["cache-save", "ref"])

    def test_the_scanner_reads_the_aspect_database_and_hedron_is_not_used(self):
        commands = "\n".join(step.get("run", "") for step in self.steps)
        self.assertIn("bazel run --config=sonar_inputs //:sonar_inputs", commands)
        self.assertNotIn("--qualification-records", commands)
        self.assertNotIn("compile_commands:refresh", commands)
        properties = (ROOT / "sonar-project.properties").read_text()
        self.assertIn(
            "sonar.cfamily.compile-commands=sonar-inputs/compile_commands.json", properties
        )

    def test_a_transient_scan_failure_is_retried_once_before_the_gate(self):
        first = self.steps[self.index("sonarcloud")]
        retry = self.steps[self.index("sonarcloud_retry")]
        self.assertTrue(first["continue-on-error"])
        self.assertEqual(retry["if"], "steps.sonarcloud.outcome == 'failure'")
        self.assertFalse(retry.get("continue-on-error", False))
        self.assertEqual(first["uses"], retry["uses"])
        self.assertEqual(first["env"], retry["env"])
        self.assertEqual(first.get("with"), retry.get("with"))
        gate = self.steps[self.named("Check SonarCloud Quality Gate")]
        self.assertFalse(gate.get("continue-on-error", False))


if __name__ == "__main__":
    unittest.main()
