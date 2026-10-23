"""Protect the Sonar workflow's single-job ordering, retry and revision boundaries."""

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

    def test_one_job_builds_coverage_then_prepares_imports_and_scans(self):
        self.assertEqual(list(self.jobs), ["sonarcloud"])
        self.assertNotIn("needs", self.jobs["sonarcloud"])
        order = [
            self.named("Build and run tests with coverage"),
            self.named("Prepare configured scanner inputs"),
            self.index("coverage_import"),
            self.index("sonarcloud"),
            self.index("sonarcloud_retry"),
            self.named("Check SonarCloud Quality Gate"),
        ]
        self.assertEqual(order, sorted(order))

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

    def test_both_boundaries_use_the_one_checked_out_commit(self):
        checkout = next(
            step for step in self.steps if step.get("uses", "").startswith("actions/checkout@")
        )
        self.assertEqual(checkout["with"]["ref"], "${{ inputs.ref }}")
        for identifier in ("coverage_export", "coverage_import"):
            run = self.steps[self.index(identifier)]["run"]
            self.assertIn("$(git rev-parse HEAD)", run)
            self.assertIn('"$GITHUB_RUN_ID"', run)
        self.assertNotIn("--attempt", self.steps[self.index("coverage_import")]["run"])

    def test_rerun_reuses_the_earlier_successful_coverage_product(self):
        restore = self.steps[self.index("coverage_cache")]
        self.assertTrue(restore["uses"].startswith("actions/cache/restore@"))
        self.assertEqual(restore["if"], "github.run_attempt > 1")
        self.assertIn("github.run_id", restore["with"]["key"])
        self.assertNotIn("github.run_attempt", restore["with"]["key"])
        fresh = "steps.coverage_cache.outputs.cache-hit != 'true'"
        save = self.steps[self.index("coverage_save")]
        self.assertTrue(save["uses"].startswith("actions/cache/save@"))
        self.assertEqual(save["with"]["key"], restore["with"]["key"])
        self.assertEqual(save["with"]["path"], restore["with"]["path"])
        for step in (
            self.steps[self.named("Build and run tests with coverage")],
            self.steps[self.index("coverage_export")],
            save,
        ):
            self.assertEqual(step["if"], fresh)
        self.assertLess(self.index("coverage_export"), self.index("coverage_save"))
        self.assertLess(self.index("coverage_save"), self.named("Prepare configured"))
        self.assertLess(self.index("coverage_cache"), self.named("Build and run tests"))

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

    def test_coverage_is_executed_once_per_fresh_attempt(self):
        runs = [step.get("run", "") for step in self.steps]
        self.assertEqual(sum("coverage-local.sh" in run for run in runs), 1)


if __name__ == "__main__":
    unittest.main()
