"""OpenFOAM case discovery and validation."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Case:
    name: str
    working_dir: str


def discover_cases(root: Path) -> list[Case]:
    """Find a single case or immediate child case directories under ``root``.

    A case is recognized by ``system/controlDict``. Independent cases become
    independent tasks; this intentionally models parameter sweeps, not a single
    MPI-coupled OpenFOAM solve.
    """
    root = root.resolve()
    if not root.is_dir():
        raise FileNotFoundError(f"case directory not found: {root}")

    if (root / "system" / "controlDict").is_file():
        return [Case(name=root.name or "case", working_dir=".")]

    cases = [
        Case(name=child.name, working_dir=child.name)
        for child in sorted(root.iterdir())
        if child.is_dir() and not child.name.startswith(".")
        and (child / "system" / "controlDict").is_file()
    ]
    if not cases:
        raise ValueError(
            f"no OpenFOAM cases found under {root}; expected system/controlDict "
            "in the root or an immediate child directory"
        )
    return cases
