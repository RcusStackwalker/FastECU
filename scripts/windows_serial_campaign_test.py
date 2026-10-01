"""The campaign must reject failures, cached runs, and incomplete evidence."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class CampaignTest(unittest.TestCase):
    def run_report(self, statuses, *, code=0, cached=False, runs=2, label="//serial:example"):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "targets.txt").write_text(label + "\n")
            events = []
            for run, status in enumerate(statuses, 1):
                log = root / f"source-{run}.log"
                log.write_text(f"attempt {run}: {status}\n")
                xml = root / f"source-{run}.xml"
                xml.write_text(
                    '<testsuites><testsuite><testcase name="works"/></testsuite></testsuites>'
                )
                events.append(
                    {
                        "id": {
                            "testResult": {
                                "label": label,
                                "run": run,
                                "attempt": 1,
                                "shard": 1,
                            }
                        },
                        "testResult": {
                            "status": status,
                            "cachedLocally": cached,
                            "testActionOutput": [
                                {"name": "test.log", "uri": log.as_uri()},
                                {"name": "test.xml", "uri": xml.as_uri()},
                            ],
                        },
                    }
                )
            events.append(
                {
                    "id": {"buildFinished": {}},
                    "finished": {"exitCode": {"code": code} if code else {"name": "SUCCESS"}},
                }
            )
            (root / "events.jsonl").write_text("".join(json.dumps(e) + "\n" for e in events))
            result = subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).with_name("windows_serial_campaign.py")),
                    str(root),
                    "--runs",
                    str(runs),
                    "--exit-code",
                    str(code),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertTrue((root / "summary.md").exists(), result.stderr)
            summary = (root / "summary.md").read_text()
            evidence = list((root / "attempts").rglob("test.log"))
            return result.returncode, summary, len(evidence)

    def test_complete_fresh_runs_pass(self):
        code, summary, logs = self.run_report(["PASSED", "PASSED"])
        self.assertEqual(code, 0)
        self.assertIn("not reproduced", summary)
        self.assertEqual(logs, 2)

    def test_failure_followed_by_pass_stays_red_and_keeps_both_logs(self):
        code, summary, logs = self.run_report(["FAILED", "PASSED"])
        self.assertEqual(code, 1)
        self.assertIn("failures observed", summary)
        self.assertEqual(logs, 2)

    def test_root_package_label_keeps_evidence_under_output_directory(self):
        code, _, logs = self.run_report(["PASSED", "PASSED"], label="//:example")
        self.assertEqual(code, 0)
        self.assertEqual(logs, 2)

    def test_missing_run_is_inconclusive(self):
        code, summary, _ = self.run_report(["PASSED"])
        self.assertEqual(code, 1)
        self.assertIn("inconclusive", summary)

    def test_cache_hits_do_not_count_as_verification(self):
        code, summary, _ = self.run_report(["PASSED", "PASSED"], cached=True)
        self.assertEqual(code, 1)
        self.assertIn("cached", summary)

    def test_build_failure_does_not_become_a_clean_campaign(self):
        code, summary, _ = self.run_report([], code=1)
        self.assertEqual(code, 1)
        self.assertIn("inconclusive", summary)

    def test_timeout_is_inconclusive_not_a_reproduced_crash(self):
        code, summary, _ = self.run_report(["TIMEOUT", "PASSED"], code=3)
        self.assertEqual(code, 1)
        self.assertIn("inconclusive", summary)


if __name__ == "__main__":
    unittest.main()
