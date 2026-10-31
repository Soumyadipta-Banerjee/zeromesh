"""Harness for integration tests: runs real zmd/zm binaries from the build directory.

The build directory comes from $ZM_BUILD_DIR (set by `meson test`), defaulting to ../../build.
Each Zmd gets its own temporary ZM_STATE_DIR, ZM_RUNTIME_DIR and ZM_CONFIG_DIR.
"""

import os
import shutil
import socket
import subprocess
import tempfile
import time

BUILD_DIR = os.environ.get(
    "ZM_BUILD_DIR",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build"))
ZMD = os.path.join(BUILD_DIR, "zmd")
ZM = os.path.join(BUILD_DIR, "zm")


def socket_alive(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path)
        return True
    except OSError:
        return False
    finally:
        s.close()


class Zmd:
    """One zmd process. `ctx` (a unittest.TestCase) gets a cleanup that stops it."""

    def __init__(self, ctx=None, state_dir=None, env=None):
        self.root = tempfile.mkdtemp(prefix="zmt-")  # short: unix socket paths are limited
        self.state_dir = state_dir or os.path.join(self.root, "state")
        self.runtime_dir = os.path.join(self.root, "run")
        self.socket = os.path.join(self.runtime_dir, "zmd.sock")
        self.env = dict(os.environ)
        self.env.update({
            "ZM_STATE_DIR": self.state_dir,
            "ZM_RUNTIME_DIR": self.runtime_dir,
            "ZM_CONFIG_DIR": os.path.join(self.root, "config"),
            "ZM_PLUGIN_PATH": "",
        })
        self.env.update(env or {})
        # The comfort governor reacts to the host's real PSI, so a busy desktop would freeze the
        # test's donors and make results depend on what else the machine is doing. Tests that
        # want it supply their own donor.toml before the daemon starts.
        config_dir = self.env["ZM_CONFIG_DIR"]
        os.makedirs(config_dir, exist_ok=True)
        donor_toml = os.path.join(config_dir, "donor.toml")
        if not os.path.exists(donor_toml):
            with open(donor_toml, "w") as f:
                f.write("# test default: comfort governor off\nmax_step = 0\n")
        self.log_path = os.path.join(self.root, "zmd.log")
        self._log = open(self.log_path, "wb")
        self.proc = subprocess.Popen([ZMD], env=self.env, stdout=self._log, stderr=subprocess.STDOUT)
        if ctx is not None:
            ctx.addCleanup(self.close)
        deadline = time.monotonic() + 5
        while not socket_alive(self.socket):
            if self.proc.poll() is not None or time.monotonic() > deadline:
                self.close()
                raise RuntimeError("zmd did not start:\n" + self.log())
            time.sleep(0.02)

    def log(self):
        with open(self.log_path, "r", errors="replace") as f:
            return f.read()

    def zm(self, *args, cwd=None, timeout=60):
        """Runs `zm --socket <sock> args...` and returns the CompletedProcess (text mode)."""
        return subprocess.run([ZM, "--socket", self.socket, *args], cwd=cwd, env=self.env,
                              capture_output=True, text=True, timeout=timeout)

    def run_another_zmd(self, timeout=10):
        """Starts a second zmd with this one's environment and waits for it to exit."""
        return subprocess.run([ZMD], env=self.env, capture_output=True, text=True, timeout=timeout)

    def stop(self):
        """SIGTERM, then wait. Returns zmd's exit code."""
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        return self.proc.returncode

    def close(self):
        self.stop()
        self._log.close()
        shutil.rmtree(self.root, ignore_errors=True)
