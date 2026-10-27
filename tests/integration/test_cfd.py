"""Two-node OpenFOAM adapter smoke test using a deterministic fake solver."""

from __future__ import annotations

import os
import re
import socket
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import zmtest  # noqa: E402
import zeromesh  # noqa: E402
from zm_cfd.runner import CfdOptions, run_cfd  # noqa: E402


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def node_id_of(zmd) -> str:
    return Path(zmd.state_dir, "node_id").read_text().strip()


def wait_until(predicate, timeout: float = 20.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.25)
    return False


class CfdMeshTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        discovery_port = free_port()
        build_dir = zmtest.BUILD_DIR
        cls.a = zmtest.Zmd(env={
            "ZM_NODE_PORT": str(free_port()),
            "ZM_DISCOVERY_PORT": str(discovery_port),
            "ZM_PLUGIN_PATH": build_dir,
        })
        cls.b = zmtest.Zmd(env={
            "ZM_NODE_PORT": str(free_port()),
            "ZM_DISCOVERY_PORT": str(discovery_port),
            "ZM_PLUGIN_PATH": build_dir,
        })

        created = cls.a.zm("team", "create")
        if created.returncode:
            raise RuntimeError("team create failed:\n" + created.stderr + "\n" + cls.a.log())
        phrase = re.search(r"'([^']+)'", created.stdout)
        if not phrase:
            raise RuntimeError("team phrase missing: " + created.stdout)
        joined = cls.b.zm("team", "join", phrase.group(1))
        if joined.returncode:
            raise RuntimeError("team join failed:\n" + joined.stderr + "\n" + cls.b.log())

        cls.id_a = node_id_of(cls.a)
        cls.id_b = node_id_of(cls.b)

        def donor_admitted() -> bool:
            status = cls.a.zm("status")
            return status.returncode == 0 and any(
                cls.id_b[:8] in line and "yes" in line.split()
                for line in status.stdout.splitlines()
            )

        if not wait_until(donor_admitted):
            raise RuntimeError(
                "donor was not admitted\nA status:\n" + cls.a.zm("status").stdout
                + "\nA log:\n" + cls.a.log() + "\nB log:\n" + cls.b.log()
            )

    @classmethod
    def tearDownClass(cls):
        for daemon in (getattr(cls, "a", None), getattr(cls, "b", None)):
            if daemon:
                daemon.close()

    def test_case_sweep_runs_on_donor_and_collects_outputs(self):
        with tempfile.TemporaryDirectory(prefix="zm-cfd-") as temp:
            root = Path(temp) / "cases"
            root.mkdir()
            fake_solver = root / "fakeFoam"
            fake_solver.write_text(
                "#!/usr/bin/env python3\n"
                "import os\n"
                "from pathlib import Path\n"
                "out = Path('postProcessing') / 'summary.txt'\n"
                "out.parent.mkdir(parents=True, exist_ok=True)\n"
                "out.write_text('node=' + os.environ.get('ZM_NODE_ID', '?') + '\\n')\n"
            )
            fake_solver.chmod(0o755)
            for case_name in ("lowRe", "highRe"):
                system = root / case_name / "system"
                system.mkdir(parents=True)
                (system / "controlDict").write_text("application fakeFoam;\n")

            output = Path(temp) / "results"
            client = zeromesh.connect(socket_path=self.a.socket)
            with client:
                result = run_cfd(
                    client,
                    CfdOptions(
                        cases=root,
                        solver="../fakeFoam",
                        out_dir=output,
                        may_run_local=False,
                    ),
                )

            self.assertEqual(result.failed, [])
            self.assertEqual({item.executed_by for item in result.cases}, {self.id_b})
            for case_name in ("lowRe", "highRe"):
                summary = output / case_name / "postProcessing" / "summary.txt"
                self.assertEqual(summary.read_text().strip(), "node=" + self.id_b)


if __name__ == "__main__":
    unittest.main()
