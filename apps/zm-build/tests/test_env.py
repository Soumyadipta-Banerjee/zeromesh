import tempfile
import unittest
from pathlib import Path

from zm_build.env import (
    apply_make_launchers,
    detect_project,
    recc_env,
)


class ReccEnvTest(unittest.TestCase):
    def test_sets_servers_and_launchers(self):
        env = recc_env(server="127.0.0.1:9000", project_root=Path("/tmp/proj"), base={})
        self.assertEqual(env["RECC_SERVER"], "127.0.0.1:9000")
        self.assertEqual(env["RECC_CAS_SERVER"], "127.0.0.1:9000")
        self.assertEqual(env["RECC_ACTION_CACHE_SERVERS"], "127.0.0.1:9000")
        self.assertEqual(env["CMAKE_CXX_COMPILER_LAUNCHER"], "recc")
        self.assertEqual(env["CMAKE_C_COMPILER_LAUNCHER"], "recc")
        self.assertEqual(env["RECC_PROJECT_ROOT"], str(Path("/tmp/proj").resolve()))

    def test_strips_recc_link(self):
        env = recc_env(base={"RECC_LINK": "1", "PATH": "/bin"})
        self.assertNotIn("RECC_LINK", env)
        self.assertIn("PATH", env)


class MakeLaunchersTest(unittest.TestCase):
    def test_wraps_defaults(self):
        env = {"PATH": "/bin"}
        apply_make_launchers(env)
        self.assertEqual(env["CC"], "recc cc")
        self.assertEqual(env["CXX"], "recc c++")

    def test_idempotent(self):
        env = {"CC": "recc clang", "CXX": "recc clang++"}
        apply_make_launchers(env)
        self.assertEqual(env["CC"], "recc clang")
        self.assertEqual(env["CXX"], "recc clang++")


class DetectProjectTest(unittest.TestCase):
    def test_cmake(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, "CMakeLists.txt").write_text("project(x)\n")
            self.assertEqual(detect_project(Path(d)), "cmake")

    def test_make(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, "Makefile").write_text("all:\n\ttrue\n")
            self.assertEqual(detect_project(Path(d)), "make")

    def test_missing(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(FileNotFoundError):
                detect_project(Path(d))


if __name__ == "__main__":
    unittest.main()
