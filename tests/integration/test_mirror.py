"""M5c acceptance: env-mirror pulls the requester's tree lazily on a remote donor."""

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


def fuse_available():
    return os.path.exists("/dev/fuse") and os.access("/dev/fuse", os.R_OK | os.W_OK)


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


def wait_until(pred, timeout=20.0, interval=0.25):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return False


class MirrorIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not fuse_available():
            raise unittest.SkipTest("FUSE unavailable (/dev/fuse)")

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

        if not wait_until(a_sees_b_admitted, timeout=25):
            raise RuntimeError(
                "A did not discover/admit B within 25s\n"
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

    def test_mirror_remote_only_tool_on_requester(self):
        """B runs a binary that exists only on A, exported via --mirror."""
        job_cwd = tempdir(self)
        mirror_root = tempdir(self)
        bin_dir = os.path.join(mirror_root, "bin")
        os.makedirs(bin_dir, exist_ok=True)
        tool = os.path.join(bin_dir, "a_only")
        with open(tool, "w") as f:
            f.write("#!/bin/sh\n")
            f.write('echo "mirror-ok" > out.txt\n')
        os.chmod(tool, 0o755)

        secret = os.path.join(mirror_root, ".ssh", "id_test")
        os.makedirs(os.path.dirname(secret), exist_ok=True)
        with open(secret, "w") as f:
            f.write("must-not-leak\n")

        r = self.a.zm(
            "run",
            "--remote-only",
            "--env",
            "mirror",
            "--mirror",
            mirror_root,
            "-o",
            "out.txt",
            "--",
            "/mirror/bin/a_only",
            cwd=job_cwd,
            timeout=120,
        )
        self.assertEqual(
            r.returncode,
            0,
            r.stderr + "\nA log:\n" + self.a.log() + "\nB log:\n" + self.b.log(),
        )

        out_file = os.path.join(job_cwd, "out.txt")
        self.assertTrue(os.path.exists(out_file))
        with open(out_file) as f:
            self.assertEqual(f.read().strip(), "mirror-ok")

        probe = self.a.zm(
            "run",
            "--remote-only",
            "--env",
            "mirror",
            "--mirror",
            mirror_root,
            "--",
            "sh",
            "-c",
            "test ! -e /mirror/.ssh/id_test",
            cwd=job_cwd,
            timeout=120,
        )
        self.assertEqual(probe.returncode, 0, probe.stderr)

        cas_b = os.path.join(self.b.state_dir, "cas")
        before = sum(
            os.path.getsize(os.path.join(cas_b, name))
            for name in os.listdir(cas_b)
            if os.path.isfile(os.path.join(cas_b, name))
        )

        r2 = self.a.zm(
            "run",
            "--remote-only",
            "--env",
            "mirror",
            "--mirror",
            mirror_root,
            "-o",
            "out.txt",
            "--",
            "/mirror/bin/a_only",
            cwd=job_cwd,
            timeout=120,
        )
        self.assertEqual(r2.returncode, 0, r2.stderr)

        after = sum(
            os.path.getsize(os.path.join(cas_b, name))
            for name in os.listdir(cas_b)
            if os.path.isfile(os.path.join(cas_b, name))
        )
        self.assertEqual(before, after, "second run should reuse B's CAS cache (no new blob files)")


if __name__ == "__main__":
    unittest.main()
