"""Read actual aspect metadata through a generated and transitioned graph."""

import json
import sys
import unittest
from pathlib import Path

from python.runfiles import runfiles

RUNFILES = runfiles.Create()
FRAGMENTS = [
    Path(RUNFILES.Rlocation("_main/" + path))
    for path in sys.argv[1:]
    if path.endswith(".sonar.json")
]
HEADERS = [
    Path(RUNFILES.Rlocation("_main/" + path))
    for path in sys.argv[1:]
    if path.endswith("/generated.h")
]


class GraphOutputsTest(unittest.TestCase):
    def test_generated_inputs_and_both_configurations_are_present(self):
        fragments = [json.loads(path.read_text()) for path in FRAGMENTS]
        probes = [
            (fragment, command)
            for fragment in fragments
            for command in fragment["commands"]
            if command["file"].endswith("/probe.cpp")
        ]
        self.assertEqual(len(probes), 2)
        self.assertEqual(len({fragment["configuration"] for fragment, _ in probes}), 2)
        self.assertEqual(
            sum("-DFASTECU_TRANSITION=1" in command["arguments"] for _, command in probes), 1
        )
        for _, command in probes:
            self.assertIn("-DFASTECU_NAME=two words", command["arguments"])
            self.assertFalse(any(argument.startswith("@") for argument in command["arguments"]))
        generated = [
            command
            for fragment in fragments
            for command in fragment["commands"]
            if command["file"].endswith("/generated.cpp")
        ]
        self.assertEqual(len(generated), 2)
        self.assertEqual(len(HEADERS), 2)
        self.assertTrue(all(header.is_file() for header in HEADERS))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
