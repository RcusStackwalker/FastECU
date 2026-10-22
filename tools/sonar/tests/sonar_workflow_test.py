"""Protect the candidate workflow's artifact and revision handoff boundaries."""

import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).parents[3]


class SonarWorkflowTest(unittest.TestCase):
    def test_metadata_graph_regression_runs_on_the_scanner_host(self):
        commands = "\n".join(step.get("run", "") for step in self.jobs["sonarcloud"]["steps"])
        self.assertIn("//tools/sonar:graph_outputs_test", commands)

    def setUp(self):
        workflow = yaml.safe_load((ROOT / ".github/workflows/sonar-qualification.yml").read_text())
        self.jobs = workflow["jobs"]
        self.permissions = workflow["permissions"]

    def test_scanner_prepares_in_parallel_then_waits_for_the_successful_producer(self):
        producer = self.jobs["coverage"]
        scanner = self.jobs["sonarcloud"]
        self.assertNotIn("needs", scanner)
        identifiers = [step.get("id") for step in scanner["steps"]]
        names = [step.get("name", "") for step in scanner["steps"]]
        prepare = next(i for i, name in enumerate(names) if name.startswith("Prepare configured"))
        wait = identifiers.index("producer")
        self.assertLess(prepare, wait)
        self.assertLess(wait, identifiers.index("coverage_download"))
        run = scanner["steps"][wait]["run"]
        self.assertIn("success", run)
        for failed in ("failure", "cancelled", "timed_out"):
            self.assertIn(failed, run)
        download = next(step for step in scanner["steps"] if step.get("id") == "coverage_download")
        self.assertEqual(
            download["with"]["artifact-ids"], "${{ steps.producer.outputs.coverage_id }}"
        )
        checkout = next(
            step
            for step in scanner["steps"]
            if step.get("uses", "").startswith("actions/checkout@")
        )
        self.assertEqual(checkout["with"]["ref"], "${{ inputs.ref || github.sha }}")
        upload = next(step for step in producer["steps"] if step.get("id") == "coverage_upload")
        self.assertEqual(upload["with"]["if-no-files-found"], "error")
        self.assertIn("github.run_id", upload["with"]["name"])
        self.assertIn("github.run_attempt", upload["with"]["name"])
        self.assertIn("actions: read", yaml.dump(self.permissions))

    def test_scan_waits_for_import_and_does_not_reexecute_coverage(self):
        steps = self.jobs["sonarcloud"]["steps"]
        identifiers = [step.get("id") for step in steps]
        self.assertLess(identifiers.index("coverage_import"), identifiers.index("sonarcloud"))
        for step in steps:
            run = step.get("run", "")
            self.assertNotIn("coverage-local.sh", run)
            if step.get("id") == "coverage_import":
                self.assertNotIn("--attempt", run)
                self.assertIn('"$GITHUB_RUN_ID"', run)
                self.assertIn("git rev-parse HEAD", run)
        gate = next(
            step for step in steps if "sonarqube-quality-gate-action@" in step.get("uses", "")
        )
        self.assertFalse(gate.get("continue-on-error", False))


if __name__ == "__main__":
    unittest.main()
