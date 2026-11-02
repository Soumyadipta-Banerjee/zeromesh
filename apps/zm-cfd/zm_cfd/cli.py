"""zm-cfd: independent OpenFOAM cases across a ZeroMesh team, one item per case."""

from __future__ import annotations

import shlex
from pathlib import Path

from zeromesh.adapter import Adapter, AdapterError, Item, Out, Policy, main as run

from zm_cfd.cases import discover_cases


def solver_command(solver: str, solver_args: list[str], bashrc: Path | None) -> list[str]:
    """The solver's argv; with an OpenFOAM setup script, sourced first in a login shell."""
    command = [solver, *solver_args]
    if bashrc is None:
        return command
    return ["bash", "-lc", f"source {shlex.quote(str(bashrc))} && exec {shlex.join(command)}"]


class Cfd(Adapter):
    name = "zm-cfd"
    kind = "openfoam"
    policy = Policy(keep_going=True)  # cases are independent

    def add_args(self, p):
        p.add_argument("cases", type=Path, help="one OpenFOAM case, or a directory of case directories")
        p.add_argument("--solver", default="simpleFoam", help="solver executable (default: simpleFoam)")
        p.add_argument("--solver-arg", action="append", default=[], help="solver argument; repeat as needed")
        p.add_argument("--bashrc", type=Path, help="OpenFOAM setup script each case sources first")

    def check(self, a):
        try:
            self.cases = discover_cases(a.cases)
        except (FileNotFoundError, ValueError) as e:
            raise AdapterError(str(e)) from None

    def workspace(self, a):
        return str(a.cases.resolve())

    def items(self, a):
        argv = solver_command(a.solver, a.solver_arg, a.bashrc)
        for case in self.cases:
            # Whatever the solver writes (time directories, postProcessing/) lands in <out>/<case>.
            dest = "" if case.working_dir == "." else case.working_dir
            yield Item(id=case.name, argv=argv, cwd="" if case.working_dir == "." else case.working_dir,
                       outputs=[Out.changed(dest)])


def main(argv: list[str] | None = None) -> None:
    run(Cfd, argv)


if __name__ == "__main__":
    main()
