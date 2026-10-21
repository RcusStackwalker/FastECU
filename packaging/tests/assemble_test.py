import os
import stat
import tempfile
import unittest
import zipfile

from packaging import assemble


@unittest.skipIf(os.name == "nt", "needs symlinks and a shell stand-in for the deploy tool")
class ZipTreeTest(unittest.TestCase):
    def _zip(self, root):
        out = os.path.join(tempfile.mkdtemp(), "out.zip")
        assemble.zip_tree(root, out)
        return out

    def test_keeps_symlinks_and_exec_bits(self):
        root = tempfile.mkdtemp()
        os.makedirs(os.path.join(root, "fw", "Versions", "A"))
        with open(os.path.join(root, "fw", "Versions", "A", "lib"), "w") as f:
            f.write("x")
        os.symlink("A", os.path.join(root, "fw", "Versions", "Current"))
        tool = os.path.join(root, "tool")
        with open(tool, "w") as f:
            f.write("#!/bin/sh\n")
        os.chmod(tool, 0o755)
        with zipfile.ZipFile(self._zip(root)) as zf:
            link = zf.getinfo("fw/Versions/Current")
            self.assertTrue(stat.S_ISLNK(link.external_attr >> 16))
            self.assertEqual(zf.read("fw/Versions/Current"), b"A")
            self.assertTrue(zf.getinfo("tool").external_attr >> 16 & stat.S_IXUSR)

    def test_same_inputs_give_same_bytes(self):
        root = tempfile.mkdtemp()
        for name in ("b", "a"):
            with open(os.path.join(root, name), "w") as f:
                f.write(name)
        with open(self._zip(root), "rb") as f1, open(self._zip(root), "rb") as f2:
            self.assertEqual(f1.read(), f2.read())


@unittest.skipIf(os.name == "nt", "needs symlinks and a shell stand-in for the deploy tool")
class WindowsAssembleTest(unittest.TestCase):
    def _fake_tool(self, dll_name):
        tool = os.path.join(tempfile.mkdtemp(), "deploy")
        with open(tool, "w") as f:
            f.write(f'#!/bin/sh\ntouch "$(dirname "$1")/{dll_name}"\n')
        os.chmod(tool, 0o755)
        return tool

    def _run(self, dll_name):
        work = tempfile.mkdtemp()
        app = os.path.join(work, "app")
        bridge = os.path.join(work, "bridge")
        for p in (app, bridge):
            with open(p, "w") as f:
                f.write("bin")
        out = os.path.join(work, "out.zip")
        assemble.main(
            [
                "--kind",
                "windows",
                "--app",
                app,
                "--deploy-tool",
                self._fake_tool(dll_name),
                "--extra",
                bridge + "=j2534_bridge_host.exe",
                "--out",
                out,
            ]
        )
        return out

    def test_bundles_app_bridge_and_runtime(self):
        with zipfile.ZipFile(self._run("Qt6Core.dll")) as zf:
            self.assertEqual(
                sorted(zf.namelist()), ["FastECU.exe", "Qt6Core.dll", "j2534_bridge_host.exe"]
            )

    def test_fails_when_runtime_not_staged(self):
        with self.assertRaises(SystemExit):
            self._run("other.dll")


if __name__ == "__main__":
    unittest.main()
