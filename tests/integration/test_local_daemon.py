"""M1/M2 acceptance: a real zmd with ClientApi on a unix socket, the zm CLI, local execution."""

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

    def test_python_three_task_job(self):
        import grpc
        from zeromesh.v1 import client_pb2, client_pb2_grpc, common_pb2

        host = common_pb2.EnvSpec(name="host")
        a = common_pb2.TaskSpec(name="a", argv=["sh", "-c", "echo alpha > a.txt"], outputs=["a.txt"], env_spec=host)
        b = common_pb2.TaskSpec(name="b", argv=["sh", "-c", "echo beta > b.txt"], outputs=["b.txt"], env_spec=host)
        c = common_pb2.TaskSpec(
            name="c", argv=["cat", "from_a/a.txt", "from_b/b.txt"], env_spec=host,
            inputs_from=[common_pb2.InputFrom(task="a", path="from_a"),
                         common_pb2.InputFrom(task="b", path="from_b")])
        with grpc.insecure_channel("unix:" + self.zmd.socket) as channel:
            stub = client_pb2_grpc.ClientApiStub(channel)
            job_id = stub.SubmitJob(common_pb2.JobSpec(name="three", tasks=[a, b, c]), timeout=10).job_id
            events = list(stub.WatchJob(client_pb2.JobRef(job_id=job_id), timeout=60))
            status = stub.GetJob(client_pb2.JobRef(job_id=job_id), timeout=10)

        self.assertTrue(status.done)
        for name in "abc":
            self.assertEqual(status.tasks[name], common_pb2.SUCCEEDED, status)
        logs = b"".join(e.log_chunk for e in events if e.task_name == "c" and e.log_fd == 1)
        self.assertEqual(logs, b"alpha\nbeta\n")
        final = [e for e in events if e.task_name == "c" and e.state == common_pb2.SUCCEEDED]
        self.assertEqual(len(final), 1)
        self.assertEqual(final[0].result.stdout_head, b"alpha\nbeta\n")
        self.assertEqual(final[0].result.exit_code, 0)


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
