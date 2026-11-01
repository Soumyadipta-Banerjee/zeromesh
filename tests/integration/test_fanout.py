"""M4 acceptance (single machine): three zmd processes, --each fan-out, SIGKILL retry."""

import hashlib
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
    d = tempfile.mkdtemp(prefix="zmfo-")
    ctx.addCleanup(shutil.rmtree, d, True)
    return d


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


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


class FanoutMesh(unittest.TestCase):
    """A submits; B and C donate. Shared discovery port, distinct node ports."""

    @classmethod
    def setUpClass(cls):
        cls.discovery_port = free_port()
        cls.port_a = free_port()
        cls.port_b = free_port()
        cls.port_c = free_port()

        def env(port):
            return {
                "ZM_NODE_PORT": str(port),
                "ZM_DISCOVERY_PORT": str(cls.discovery_port),
            }

        cls.a = zmtest.Zmd(env=env(cls.port_a))
        cls.b = zmtest.Zmd(env=env(cls.port_b))
        cls.c = zmtest.Zmd(env=env(cls.port_c))

        create = cls.a.zm("team", "create")
        if create.returncode != 0:
            raise RuntimeError("team create failed:\n" + create.stderr + "\n" + cls.a.log())
        m = re.search(r"'([^']+)'", create.stdout)
        if not m:
            raise RuntimeError("no phrase in team create stdout: " + repr(create.stdout))
        cls.phrase = m.group(1)

        for z in (cls.b, cls.c):
            join = z.zm("team", "join", cls.phrase)
            if join.returncode != 0:
                raise RuntimeError("team join failed:\n" + join.stderr + "\n" + z.log())

        cls.id_a = node_id_of(cls.a)
        cls.id_b = node_id_of(cls.b)
        cls.id_c = node_id_of(cls.c)

        def a_sees_donors():
            r = cls.a.zm("status")
            if r.returncode != 0:
                return False
            lines = r.stdout.splitlines()
            ok_b = any(cls.id_b[:8] in line and "yes" in line.split() for line in lines)
            ok_c = any(cls.id_c[:8] in line and "yes" in line.split() for line in lines)
            return ok_b and ok_c

        if not wait_until(a_sees_donors, timeout=25):
            raise RuntimeError(
                "A did not admit B and C within 25s\n"
                f"A status:\n{cls.a.zm('status').stdout}\n"
                f"A log:\n{cls.a.log()}\nB log:\n{cls.b.log()}\nC log:\n{cls.c.log()}"
            )

    @classmethod
    def tearDownClass(cls):
        for z in (cls.a, cls.b, cls.c):
            try:
                z.close()
            except Exception:
                pass

    def test_each_fanout_survives_sigkill(self):
        cwd = tempdir(self)
        cases = os.path.join(cwd, "cases")
        os.makedirs(cases)
        expected = {}
        for i in range(20):
            path = os.path.join(cases, f"case{i:02d}")
            data = f"payload-{i}\n".encode()
            with open(path, "wb") as f:
                f.write(data)
            expected[f"cases/case{i:02d}"] = hashlib.sha256(data).hexdigest()

        # Run in a background-friendly way: start zm, wait ~4s, SIGKILL one donor.
        import subprocess

        cmd = [
            zmtest.ZM,
            "--socket",
            self.a.socket,
            "run",
            "--remote-only",
            "--each",
            "cases/*",
            "--",
            "sh",
            "-c",
            "sleep 8; sha256sum {} > {}.out",
        ]
        env = dict(self.a.env)
        proc = subprocess.Popen(
            cmd,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        # Wait until tasks are mid-sleep. Kill B first; if placement put nothing on B,
        # also kill C so at least one in-flight donor is gone (the survivor must finish).
        time.sleep(4.0)
        killed = []
        if self.b.proc.poll() is None:
            self.b.proc.kill()
            self.b.proc.wait(timeout=10)
            killed.append(self.id_b)

        try:
            stdout, stderr = proc.communicate(timeout=120)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
            self.fail(f"zm timed out\nstdout={stdout}\nstderr={stderr}\nA={self.a.log()}\nC={self.c.log()}")

        # If B had no in-flight work, there may be no retry — re-run is too heavy; assert
        # spread happened by requiring a retry that names the killed donor when present.
        combined = stderr + stdout + self.a.log()
        self.assertEqual(
            proc.returncode,
            0,
            f"zm exit {proc.returncode}\nstderr={stderr}\nstdout={stdout}\n"
            f"A={self.a.log()}\nB={self.b.log()}\nC={self.c.log()}",
        )

        for rel, digest in expected.items():
            out_path = os.path.join(cwd, rel + ".out")
            self.assertTrue(os.path.isfile(out_path), f"missing {out_path}")
            with open(out_path) as f:
                line = f.read().strip()
            self.assertTrue(line.startswith(digest), f"{rel}: got {line!r}, want {digest}")

        self.assertRegex(
            combined,
            r"retry 1/3",
            msg="expected a retry event after SIGKILL donor; "
                "with 20 tasks and 2 donors both should have had in-flight work",
        )
        # The lost node in the retry reason should be the donor we killed.
        self.assertTrue(
            any(kid[:12] in combined for kid in killed),
            f"retry did not mention killed donor {killed}; stderr/log:\n{combined}",
        )


if __name__ == "__main__":
    unittest.main()
