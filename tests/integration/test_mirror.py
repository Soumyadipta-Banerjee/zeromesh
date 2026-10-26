"""M5c acceptance: integration test for env-mirror.

Two zmd processes (A and B) in one team.
A runs a job with `--remote-only --env mirror -o out.txt -- ...`.
B executes the job, produces `out.txt`.
"""

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


def tempdir(ctx):
    d = tempfile.mkdtemp(prefix="zmmir-")
    ctx.addCleanup(shutil.rmtree, d, True)
    return d


def node_id_of(zmd):
    path = os.path.join(zmd.state_dir, "node_id")
    with open(path) as f:
        return f.read().strip()


def wait_until(pred, timeout=15.0, interval=0.25):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return False


class MirrorIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.discovery_port = free_port()
        cls.port_a = free_port()
        cls.port_b = free_port()

        def env(port):
            return {
                "ZM_NODE_PORT": str(port),
                "ZM_DISCOVERY_PORT": str(cls.discovery_port),
            }

        cls.a = zmtest.Zmd(env=env(cls.port_a))
        cls.b = zmtest.Zmd(env=env(cls.port_b))

        create = cls.a.zm("team", "create")
        if create.returncode != 0:
            raise RuntimeError("team create failed:\n" + create.stderr + "\n" + cls.a.log())
        m = re.search(r"'([^']+)'", create.stdout)
        if not m:
            raise RuntimeError("no phrase in team create stdout: " + repr(create.stdout))
        cls.phrase = m.group(1)

        join = cls.b.zm("team", "join", cls.phrase)
        if join.returncode != 0:
            raise RuntimeError("team join failed:\n" + join.stderr + "\n" + cls.b.log())

        cls.id_a = node_id_of(cls.a)
        cls.id_b = node_id_of(cls.b)

        def a_sees_b_admitted():
            r = cls.a.zm("status")
            if r.returncode != 0:
                return False
            short = cls.id_b[:8]
            for line in r.stdout.splitlines():
                if short in line and "yes" in line.split():
                    return True
            return False

        if not wait_until(a_sees_b_admitted, timeout=20):
            raise RuntimeError(
                "A did not discover/admit B within 20s\n"
                f"A status:\n{cls.a.zm('status').stdout}\n"
                f"A log:\n{cls.a.log()}\nB log:\n{cls.b.log()}"
            )

    @classmethod
    def tearDownClass(cls):
        for z in (cls.a, cls.b):
            try:
                z.close()
            except Exception:
                pass

    def test_mirror_remote_execution(self):
        cwd = tempdir(self)
        bin_dir = os.path.join(cwd, "bin")
        os.makedirs(bin_dir, exist_ok=True)
        script_path = os.path.join(bin_dir, "hello")
        with open(script_path, "w") as f:
            f.write("#!/bin/sh\necho \"mirror-exec-by-$ZM_NODE_ID\" > out.txt\n")
        os.chmod(script_path, 0o755)

        r = self.a.zm(
            "run", "--remote-only", "--env", "mirror", "-o", "out.txt", "--",
            "sh", "-c", "./bin/hello",
            cwd=cwd, timeout=60,
        )
        self.assertEqual(r.returncode, 0, r.stderr + "\n" + self.a.log() + "\n" + self.b.log())

        out_file = os.path.join(cwd, "out.txt")
        self.assertTrue(os.path.exists(out_file))
        with open(out_file) as f:
            content = f.read().strip()
        self.assertEqual(content, f"mirror-exec-by-{self.id_b}")


if __name__ == "__main__":
    unittest.main()
