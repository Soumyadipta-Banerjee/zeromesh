"""Captured environments: a donor runs a program it does not have, from a tree captured on the
requester (zm env capture), and zm doctor reports on the node."""

import os
import re
import shutil
import socket
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import zmtest  # noqa: E402


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def node_id_of(zmd):
    with open(os.path.join(zmd.state_dir, "node_id")) as f:
        return f.read().strip()


def wait_until(pred, timeout=20.0, interval=0.25):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return False


class TreeEnvIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.discovery_port = free_port()

        def env(port):
            return {"ZM_NODE_PORT": str(port), "ZM_DISCOVERY_PORT": str(cls.discovery_port)}

        cls.a = zmtest.Zmd(env=env(free_port()))
        cls.b = zmtest.Zmd(env=env(free_port()))

        create = cls.a.zm("team", "create")
        if create.returncode != 0:
            raise RuntimeError("team create failed:\n" + create.stderr + "\n" + cls.a.log())
        m = re.search(r"'([^']+)'", create.stdout)
        if not m:
            raise RuntimeError("no phrase in team create stdout: " + repr(create.stdout))
        join = cls.b.zm("team", "join", m.group(1))
        if join.returncode != 0:
            raise RuntimeError("team join failed:\n" + join.stderr + "\n" + cls.b.log())
        cls.id_b = node_id_of(cls.b)

        def a_sees_b_admitted():
            r = cls.a.zm("status")
            return r.returncode == 0 and any(
                cls.id_b[:8] in line and "yes" in line.split() for line in r.stdout.splitlines())

        if not wait_until(a_sees_b_admitted, timeout=25):
            raise RuntimeError("A did not discover/admit B within 25s\n"
                               f"A log:\n{cls.a.log()}\nB log:\n{cls.b.log()}")

    @classmethod
    def tearDownClass(cls):
        for z in (cls.a, cls.b):
            try:
                z.close()
            except Exception:
                pass

    def tool_dir(self):
        """A program only the requester has. Captures never take /tmp (donors use their own), so it
        lives under the build directory, which donor sandboxes do not see."""
        d = tempfile.mkdtemp(prefix="zm-tree-env-", dir=zmtest.BUILD_DIR)
        self.addCleanup(shutil.rmtree, d, True)
        os.makedirs(os.path.join(d, "bin"))
        tool = os.path.join(d, "bin", "a_only")
        with open(tool, "w") as f:
            f.write("#!/bin/sh\n")
            f.write('echo "tree-ok from $(cat ' + os.path.join(d, "share", "greeting") + ')" > out.txt\n')
        os.chmod(tool, 0o755)
        os.makedirs(os.path.join(d, "share"))
        with open(os.path.join(d, "share", "greeting"), "w") as f:
            f.write("the tree\n")
        os.makedirs(os.path.join(d, ".ssh"))
        with open(os.path.join(d, ".ssh", "id_test"), "w") as f:
            f.write("must-not-leak\n")
        return d

    def capture(self, cwd, *args):
        r = self.a.zm("env", "capture", *args, cwd=cwd, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr + "\nA log:\n" + self.a.log())
        tree = r.stdout.strip()
        self.assertRegex(tree, r"^sha256:[0-9a-f]{64}$")
        return tree, r.stderr

    def test_donor_runs_a_program_it_lacks_from_a_captured_tree(self):
        tools = self.tool_dir()
        job = tempfile.mkdtemp(prefix="zmtree-job-")
        self.addCleanup(shutil.rmtree, job, True)
        tool = os.path.join(tools, "bin", "a_only")

        # The probe runs the tool: its #! interpreter, that interpreter's loader and libraries, and
        # every file the run reads are captured. --path adds the whole tool directory, minus secrets.
        tree, err = self.capture(job, "--path", tools, "--", tool)
        self.assertIn("kept out (secret)", err)
        self.assertIn(os.path.join(tools, ".ssh"), err)  # the whole directory

        again, err2 = self.capture(job, "--path", tools, "--", tool)
        self.assertEqual(again, tree)
        self.assertIn("(unchanged)", err2)

        os.remove(os.path.join(job, "out.txt"))  # the probe wrote it here
        # Control: without the tree the donor's sandbox has no such program.
        r = self.a.zm("run", "--remote-only", "--no-cache", "-o", "out.txt", "--", tool, cwd=job, timeout=120)
        self.assertNotEqual(r.returncode, 0, "the donor ran the tool without the tree:\n" + r.stderr)
        self.assertFalse(os.path.exists(os.path.join(job, "out.txt")))

        r = self.a.zm("run", "--remote-only", "--no-cache", "--env", tree, "-o", "out.txt", "--", tool,
                      cwd=job, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr + "\nA log:\n" + self.a.log() + "\nB log:\n" + self.b.log())
        with open(os.path.join(job, "out.txt")) as f:
            self.assertEqual(f.read().strip(), "tree-ok from the tree")

        # The secret stayed on the requester.
        secret = os.path.join(tools, ".ssh", "id_test")
        probe = self.a.zm("run", "--remote-only", "--no-cache", "--env", tree, "--", "/bin/sh", "-c",
                          f"test -x {tool} && test ! -e {secret}", cwd=job, timeout=120)
        self.assertEqual(probe.returncode, 0, probe.stderr + "\nB log:\n" + self.b.log())

    def test_capture_refuses_what_donors_never_mount(self):
        r = self.a.zm("env", "capture", "--path", "/etc/passwd", timeout=60)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("never mounted from an environment", r.stderr)
        r = self.a.zm("env", "capture", "--", "definitely-not-a-program-xyz", timeout=60)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not an executable program on PATH", r.stderr)

    def test_doctor_reports_each_check(self):
        r = self.a.zm("doctor", timeout=60)
        lines = r.stdout.splitlines()
        checks = {line.split()[1] for line in lines if line[:5] in ("ok   ", "warn ", "ERROR")}
        for check in ("sandbox", "cgroup", "fuse", "strace", "team", "store"):
            self.assertIn(check, checks, r.stdout)
        self.assertEqual(r.returncode, 1 if any(line.startswith("ERROR") for line in lines) else 0, r.stdout)
        team = next(line for line in lines if line.split()[1] == "team")
        self.assertIn("1 member(s) admitted", team)


if __name__ == "__main__":
    unittest.main()
