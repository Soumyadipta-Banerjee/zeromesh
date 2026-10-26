"""M6 acceptance test: two zmd nodes in a team, CMake build through recc."""

import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

import grpc
from zeromesh.v1 import client_pb2_grpc, common_pb2

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import zmtest  # noqa: E402


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


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


class ReapiMeshTest(unittest.TestCase):
    """Node A runs frontend-reapi plugin, Node B is a donor in the same team."""

    @classmethod
    def setUpClass(cls):
        # Skip if recc or cmake is missing
        if shutil.which("recc") is None:
            raise unittest.SkipTest("recc not installed (run scripts/install_recc.sh)")
        if shutil.which("cmake") is None:
            raise unittest.SkipTest("cmake not installed")

        cls.discovery_port = free_port()
        cls.port_a = free_port()
        cls.port_b = free_port()
        cls.reapi_port = free_port()

        build_dir = zmtest.BUILD_DIR

        # Node A loads frontend-reapi and env-host, listening on cls.reapi_port
        env_a = {
            "ZM_NODE_PORT": str(cls.port_a),
            "ZM_DISCOVERY_PORT": str(cls.discovery_port),
            "ZM_PLUGIN_PATH": build_dir,
            "ZM_REAPI_PORT": str(cls.reapi_port),
            "ZM_ENABLE_REAPI": "1",
        }
        # Node B loads env-host as a donor; disables frontend-reapi to avoid port conflicts
        env_b = {
            "ZM_NODE_PORT": str(cls.port_b),
            "ZM_DISCOVERY_PORT": str(cls.discovery_port),
            "ZM_PLUGIN_PATH": build_dir,
            "ZM_ENABLE_REAPI": "0",
        }

        cls.a = zmtest.Zmd(env=env_a)
        cls.b = zmtest.Zmd(env=env_b)

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

        # Wait until A sees B admitted
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

        # Verify discovery's full donor capacity before testing placement.
        channel = grpc.insecure_channel("unix:" + cls.a.socket)
        peers = client_pb2_grpc.ClientApiStub(channel).ListPeers(common_pb2.Empty())
        donor = next((p for p in peers.peers if p.node_id == cls.id_b), None)
        if donor is None or not donor.admitted:
            raise RuntimeError("admitted donor missing from ListPeers")
        if donor.cores_free < 1:
            raise RuntimeError(f"donor reports no free cores: {donor.cores_free}/{donor.cores_total}")
        channel.close()

    @classmethod
    def tearDownClass(cls):
        for z in (getattr(cls, "a", None), getattr(cls, "b", None)):
            if z:
                try:
                    z.close()
                except Exception:
                    pass

    def test_cmake_recc_build_and_run(self):
        source_template = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "data", "reapi-hello"
        )
        test_root = tempfile.mkdtemp(prefix="reapi-test-")
        self.addCleanup(shutil.rmtree, test_root, True)

        src_dir = os.path.join(test_root, "src")
        build_dir = os.path.join(test_root, "build")
        shutil.copytree(source_template, src_dir)
        os.makedirs(build_dir, exist_ok=True)

        reapi_target = f"127.0.0.1:{self.reapi_port}"
        build_env = dict(os.environ)
        # Isolate the CMake compile test from any caller's global link setting;
        # the explicit link action below enables it only for that subprocess.
        build_env.pop("RECC_LINK", None)
        build_env.update({
            "RECC_SERVER": reapi_target,
            "RECC_CAS_SERVER": reapi_target,
            "RECC_INSTANCE": "default",
            "RECC_ACTION_CACHE_SERVERS": reapi_target,
            "RECC_PROJECT_ROOT": test_root,
            "RECC_DEPS_GLOBAL_PATHS": "1",
            "RECC_VERBOSE": "1",
        })

        # Configure CMake with recc launcher
        cmake_configure = [
            "cmake",
            "-B", build_dir,
            "-S", src_dir,
            "-DCMAKE_CXX_COMPILER_LAUNCHER=recc",
        ]
        res = subprocess.run(
            cmake_configure,
            env=build_env,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(
            res.returncode, 0,
            f"CMake configure failed:\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}\nA LOG:\n{self.a.log()}"
        )

        # Build with CMake
        # Keep independent compile actions in flight together so placement can use
        # donor capacity before local host telemetry changes between sequential jobs.
        cmake_build = ["cmake", "--build", build_dir, "--parallel", "4"]
        res = subprocess.run(
            cmake_build,
            env=build_env,
            capture_output=True,
            text=True,
            timeout=120,
        )
        self.assertEqual(
            res.returncode, 0,
            f"CMake build failed:\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}\nA LOG:\n{self.a.log()}\nB LOG:\n{self.b.log()}"
        )

        # Verify binary exists and runs
        exe_path = os.path.join(build_dir, "reapi_hello")
        self.assertTrue(os.path.isfile(exe_path), f"Binary not found: {exe_path}")
        run_res = subprocess.run([exe_path], capture_output=True, text=True, timeout=10)
        self.assertEqual(run_res.returncode, 0, f"Binary exited {run_res.returncode}:\n{run_res.stderr}")
        self.assertIn("SUCCESS", run_res.stdout)

        # This recc release leaves CMake's final link local, so submit an
        # equivalent link action through recc to exercise REAPI link pinning.
        link_res = subprocess.run(
            ["recc", "c++", "-o", "reapi_hello_recc",
             "CMakeFiles/reapi_hello.dir/main.cpp.o",
             "CMakeFiles/reapi_hello.dir/math.cpp.o",
             "CMakeFiles/reapi_hello.dir/string_util.cpp.o",
             "CMakeFiles/reapi_hello.dir/printer.cpp.o"],
            cwd=build_dir,
            env={**build_env, "RECC_LINK": "1"},
            capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(link_res.returncode, 0,
                         f"recc link failed:\n{link_res.stdout}\n{link_res.stderr}")
        linked_exe = os.path.join(build_dir, "reapi_hello_recc")
        linked_run = subprocess.run([linked_exe], capture_output=True, text=True, timeout=10)
        self.assertEqual(linked_run.returncode, 0, f"recc-linked binary failed:\n{linked_run.stderr}")
        self.assertIn("SUCCESS", linked_run.stdout)

        # Verify task placement in A's log:
        # At least one compile action must have executed_by=B
        log_a = self.a.log()
        self.assertIn(f"executed_by={self.id_b}", log_a,
                      f"Expected at least one compile action on donor {self.id_b}:\n{log_a}")
        self.assertRegex(log_a, rf"task /usr/bin/c\+\+ critical=1 executed_by={self.id_a} ",
                         f"Expected REAPI link action on requester {self.id_a}:\n{log_a}")


if __name__ == "__main__":
    unittest.main()
