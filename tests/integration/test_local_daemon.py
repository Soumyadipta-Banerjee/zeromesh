"""M1/M2 acceptance: a real zmd with ClientApi on a unix socket, the zm CLI, local execution."""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import zmtest  # noqa: E402

NOT_RUNNING = "zmd is not running (start it with: zmd)"


def tempdir(ctx):
    d = tempfile.mkdtemp(prefix="zmcwd-")
    ctx.addCleanup(shutil.rmtree, d, True)
    return d


class SharedDaemon(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.zmd = zmtest.Zmd()

    @classmethod
    def tearDownClass(cls):
        cls.zmd.close()

    def run_ok(self, *args, cwd=None):
        r = self.zmd.zm(*args, cwd=cwd or tempdir(self))
        self.assertEqual(r.returncode, 0, r.stderr)
        return r

    def test_run_echo(self):
        r = self.run_ok("run", "--", "echo", "hi")
        self.assertEqual(r.stdout, "hi\n")
        # The second run is an ActionCache hit; its stdout is replayed from the CAS.
        self.assertEqual(self.run_ok("run", "--", "echo", "hi").stdout, "hi\n")

    def test_no_false_cache_hit(self):
        cwd = tempdir(self)
        self.assertEqual(self.run_ok("run", "--", "echo", "a", cwd=cwd).stdout, "a\n")
        self.assertEqual(self.run_ok("run", "--", "echo", "b", cwd=cwd).stdout, "b\n")

    def test_exit_code(self):
        r = self.zmd.zm("run", "--", "sh", "-c", "echo oops >&2; exit 3", cwd=tempdir(self))
        self.assertEqual(r.returncode, 3)
        self.assertEqual(r.stderr, "oops\n")

    def test_outputs_written_back(self):
        cwd = tempdir(self)
        self.run_ok("run", "--", "sh", "-c", "echo x > out.txt; mkdir -p sub; echo y > sub/new.txt", cwd=cwd)
        with open(os.path.join(cwd, "out.txt")) as f:
            self.assertEqual(f.read(), "x\n")
        with open(os.path.join(cwd, "sub", "new.txt")) as f:
            self.assertEqual(f.read(), "y\n")

    def test_declared_output_only(self):
        cwd = tempdir(self)
        self.run_ok("run", "-o", "a.txt", "--", "sh", "-c", "echo 1 > a.txt; echo 2 > b.txt", cwd=cwd)
        self.assertTrue(os.path.exists(os.path.join(cwd, "a.txt")))
        self.assertFalse(os.path.exists(os.path.join(cwd, "b.txt")))

    def test_rerun_is_a_cache_hit_that_replays_output(self):
        cwd = tempdir(self)
        with open(os.path.join(cwd, "in.txt"), "w") as f:
            f.write("data\n")
        first = self.run_ok("run", "--", "sh", "-c", "cat in.txt; echo made > out.txt", cwd=cwd)
        os.remove(os.path.join(cwd, "out.txt"))
        second = self.run_ok("run", "--", "sh", "-c", "cat in.txt; echo made > out.txt", cwd=cwd)
        self.assertEqual(first.stdout, "data\n")
        self.assertEqual(second.stdout, "data\n")  # replayed from the store
        with open(os.path.join(cwd, "out.txt")) as f:
            self.assertEqual(f.read(), "made\n")  # placed again from the cache
        third = self.run_ok("run", "--no-cache", "--", "sh", "-c", "cat in.txt; echo made > out.txt", cwd=cwd)
        self.assertEqual(third.stdout, "data\n")

    def test_secrets_never_leave(self):
        cwd = tempdir(self)
        with open(os.path.join(cwd, ".env"), "w") as f:
            f.write("TOKEN=secret\n")
        os.makedirs(os.path.join(cwd, ".ssh"))
        with open(os.path.join(cwd, ".ssh", "id_ed25519"), "w") as f:
            f.write("key\n")
        r = self.run_ok("run", "--", "sh", "-c", "ls -A", cwd=cwd)
        self.assertNotIn(".env", r.stdout)
        self.assertNotIn(".ssh", r.stdout)

    def test_runs_report_and_resume(self):
        cwd = tempdir(self)
        report = os.path.join(cwd, "..", os.path.basename(cwd) + "-report.json")
        self.addCleanup(lambda: [os.remove(p) for p in (report, report.replace(".json", ".trace.json"))
                                 if os.path.exists(p)])
        self.run_ok("run", "--report", report, "--each", "*.txt", "--", "sh", "-c", "wc -c < {}",
                    cwd=self.files(cwd, {"a.txt": "aa", "b.txt": "bbb"}))
        with open(report) as f:
            data = json.load(f)
        self.assertEqual(data["run"]["succeeded"], 2)
        self.assertEqual(len(data["items"]), 2)
        run_id = data["run"]["run_id"]
        listing = self.run_ok("runs").stdout
        self.assertIn(run_id, listing)
        shown = self.run_ok("report", run_id).stdout
        self.assertIn("2 succeeded", shown)
        resumed = self.zmd.zm("resume", run_id)
        self.assertEqual(resumed.returncode, 0, resumed.stderr)
        self.assertIn("2 succeeded (2 cached)", resumed.stderr)
        explain = self.run_ok("cache", "explain", run_id, "each-0").stdout
        self.assertIn("miss", explain)

    def test_run_from_json_with_dependencies(self):
        cwd = tempdir(self)
        out = tempdir(self)
        spec = {
            "name": "three",
            "workspace_path": ".",
            "stages": [{"name": ""}],
            "items": [
                {"id": "c", "argv": ["sh", "-c", "cat a.txt b.txt > c.txt"], "after": ["a", "b"],
                 "outputs": [{"path": "c.txt", "dest": os.path.join(out, "c.txt")}]},
                {"id": "a", "argv": ["sh", "-c", "echo alpha > a.txt"], "outputs": [{"path": "a.txt"}]},
                {"id": "b", "argv": ["sh", "-c", "echo beta > b.txt"], "outputs": [{"path": "b.txt"}]},
            ],
        }
        with open(os.path.join(cwd, "run.json"), "w") as f:
            json.dump(spec, f)
        r = self.run_ok("run", "--from", "run.json", cwd=cwd)
        self.assertIn("3 succeeded", r.stderr)
        with open(os.path.join(out, "c.txt")) as f:
            self.assertEqual(f.read(), "alpha\nbeta\n")

    def files(self, cwd, contents):
        for name, text in contents.items():
            with open(os.path.join(cwd, name), "w") as f:
                f.write(text)
        return cwd


class OwnDaemon(unittest.TestCase):
    def test_node_id_persists(self):
        state = tempdir(self)
        first = zmtest.Zmd(self, state_dir=state)
        r = first.zm("status")
        self.assertEqual(r.returncode, 0, r.stderr)
        m = re.search(r"Node ID:\s+([0-9a-f]{32})", r.stdout)
        self.assertIsNotNone(m, r.stdout)
        self.assertEqual(oct(os.stat(first.socket).st_mode & 0o777), oct(0o600))
        self.assertEqual(first.stop(), 0, first.log())
        self.assertFalse(os.path.exists(first.socket))

        second = zmtest.Zmd(self, state_dir=state)
        r = second.zm("status")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(m.group(1), r.stdout)

    def test_cli_without_daemon(self):
        r = subprocess.run([zmtest.ZM, "--socket", "/nonexistent", "run", "--", "true"],
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 2)
        self.assertIn(NOT_RUNNING, r.stderr)

    def test_upload_limit(self):
        zmd = zmtest.Zmd(self, env={"ZM_UPLOAD_LIMIT_BYTES": "1000"})
        cwd = tempdir(self)
        with open(os.path.join(cwd, "big.bin"), "wb") as f:
            f.write(b"\0" * 2048)
        r = zmd.zm("run", "--", "true", cwd=cwd)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("--force-large", r.stderr)
        r = zmd.zm("run", "--force-large", "--", "true", cwd=cwd)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_second_daemon_refused(self):
        zmd = zmtest.Zmd(self)
        r = zmd.run_another_zmd()
        self.assertEqual(r.returncode, 1)
        self.assertIn("zmd is already running", r.stderr)
        self.assertEqual(zmd.zm("status").returncode, 0)  # the first one is unaffected


if __name__ == "__main__":
    unittest.main(verbosity=2)
