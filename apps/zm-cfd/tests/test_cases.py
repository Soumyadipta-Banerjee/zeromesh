from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from zm_cfd.cases import discover_cases
from zm_cfd.runner import CfdOptions, _command


def make_case(root: Path, name: str) -> None:
    system = root / name / "system"
    system.mkdir(parents=True)
    (system / "controlDict").write_text("application simpleFoam;\n")


class CaseDiscoveryTests(unittest.TestCase):
    def test_discover_sibling_cases_in_stable_order(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            make_case(root, "caseB")
            make_case(root, "caseA")
            self.assertEqual(
                [(case.name, case.working_dir) for case in discover_cases(root)],
                [("caseA", "caseA"), ("caseB", "caseB")],
            )
    def test_discover_single_case_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "system").mkdir()
            (root / "system" / "controlDict").write_text("application simpleFoam;\n")
            cases = discover_cases(root)
            self.assertEqual(len(cases), 1)
            self.assertEqual(cases[0].working_dir, ".")
    def test_missing_case_is_actionable(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, "system/controlDict"):
                discover_cases(Path(temp))
    def test_setup_command_quotes_setup_path_and_solver_args(self):
        opts = CfdOptions(
            cases=Path("cases"),
            solver="simpleFoam",
            solver_args=["-case", "case with spaces"],
            bashrc=Path("/opt/OpenFOAM/etc/bashrc"),
        )
        argv = _command(opts)
        self.assertEqual(argv[:2], ["bash", "-lc"])
        self.assertIn("source /opt/OpenFOAM/etc/bashrc", argv[2])
        self.assertIn("'case with spaces'", argv[2])


if __name__ == "__main__":
    unittest.main()
