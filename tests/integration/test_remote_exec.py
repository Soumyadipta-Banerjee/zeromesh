"""M3 acceptance (single machine): three zmd processes, team pairing, remote execution."""

import os
import re
import shutil
import socket
import sys
import tempfile
import time
import unittest

import grpc

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import zmtest  # noqa: E402

from zeromesh.v1 import common_pb2, node_pb2, node_pb2_grpc  # noqa: E402


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def tempdir(ctx):
    d = tempfile.mkdtemp(prefix="zmre-")
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


class ThreeNodeMesh(unittest.TestCase):
    """A and B share a team; C has its own. Shared discovery port, distinct node ports."""

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
        # Phrase is printed single-quoted.
        m = re.search(r"'([^']+)'", create.stdout)
        if not m:
            raise RuntimeError("no phrase in team create stdout: " + repr(create.stdout))
        cls.phrase = m.group(1)

        join = cls.b.zm("team", "join", cls.phrase)
        if join.returncode != 0:
            raise RuntimeError("team join failed:\n" + join.stderr + "\n" + cls.b.log())

        create_c = cls.c.zm("team", "create")
        if create_c.returncode != 0:
            raise RuntimeError("C team create failed:\n" + create_c.stderr)

        cls.id_a = node_id_of(cls.a)
        cls.id_b = node_id_of(cls.b)
        cls.id_c = node_id_of(cls.c)

        # Wait until A sees B admitted (discovery + admission loop).
        def a_sees_b_admitted():
            r = cls.a.zm("status")
            if r.returncode != 0:
                return False
            # Peer row starts with first 8 chars of id; ADMITTED column ends with yes/no.
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
        for z in (cls.a, cls.b, cls.c):
            try:
                z.close()
            except Exception:
                pass

    def test_peers_discovered(self):
        r = self.a.zm("status")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(self.id_b[:8], r.stdout)
        self.assertNotIn(self.id_c[:8], r.stdout)

    def test_remote_only_runs_on_b(self):
        cwd = tempdir(self)
        r = self.a.zm(
            "run", "--remote-only", "--",
            "sh", "-c", 'printf %s "$ZM_NODE_ID" > out.txt',
            cwd=cwd, timeout=60,
        )
        self.assertEqual(r.returncode, 0, r.stderr + "\n" + self.a.log() + "\n" + self.b.log())
        with open(os.path.join(cwd, "out.txt")) as f:
            self.assertEqual(f.read(), self.id_b)

    def test_remote_logs_stream(self):
        cwd = tempdir(self)
        r = self.a.zm("run", "--remote-only", "--", "sh", "-c", "echo remote-hello", cwd=cwd, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "remote-hello\n")

    def test_missing_command_remote(self):
        cwd = tempdir(self)
        r = self.a.zm(
            "run", "--remote-only", "--", "definitely-not-a-binary-xyz",
            cwd=cwd, timeout=60,
        )
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("no node can run this task", r.stderr)

    def test_bad_phrase(self):
        r = self.a.zm("team", "join", "ZM-0000")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("invalid team phrase", r.stderr)

    def test_other_team_cannot_call(self):
        # C's identity calling B's NodeApi Ping must be PERMISSION_DENIED (not pinned).
        with open(os.path.join(self.c.state_dir, "node.crt"), "rb") as f:
            c_cert = f.read()
        with open(os.path.join(self.c.state_dir, "node.key"), "rb") as f:
            c_key = f.read()
        with open(os.path.join(self.b.state_dir, "node.crt"), "rb") as f:
            b_cert = f.read()

        creds = grpc.ssl_channel_credentials(
            root_certificates=b_cert,
            private_key=c_key,
            certificate_chain=c_cert,
        )
        options = (
            ("grpc.ssl_target_name_override", self.id_b),
            ("grpc.default_authority", self.id_b),
        )
        target = f"127.0.0.1:{self.port_b}"
        with grpc.secure_channel(target, creds, options=options) as channel:
            stub = node_pb2_grpc.NodeApiStub(channel)
            try:
                stub.Ping(common_pb2.Empty(), timeout=10)
                self.fail("expected PERMISSION_DENIED")
            except grpc.RpcError as e:
                self.assertEqual(e.code(), grpc.StatusCode.PERMISSION_DENIED, str(e))


if __name__ == "__main__":
    unittest.main()
