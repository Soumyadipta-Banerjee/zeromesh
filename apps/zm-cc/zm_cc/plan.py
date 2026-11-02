"""Turn the three input forms into translation units (TUs).

Input forms, all reduced to a list of ``Entry`` (a directory plus a full command):

  --compdb FILE          compile_commands.json (CMake, Meson, Make+bear, ninja -t compdb, ...)
  SOURCES --cc CC        explicit files with one shared flag string
  -- CMD ...             one plain command line

Each source file in an entry becomes one ``Unit``. A unit is *remote* when the
command is a plain compile of that file (one item on the mesh), and *local* (run
unchanged on this machine) otherwise.
"""

from __future__ import annotations

import hashlib
import json
import os
import shlex
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional, Sequence

from zm_cc.flags import Command, Source, expand_response_files, parse

Which = Callable[[str], Optional[str]]


class PlanError(ValueError):
    """The input cannot be turned into a plan (bad compdb, duplicate outputs, ...)."""


@dataclass(frozen=True)
class Entry:
    directory: Path  # working directory of the command (relative includes resolve here)
    argv: tuple[str, ...]  # driver first


@dataclass(frozen=True)
class Unit:
    index: int
    entry: int  # the input entry (compdb row) it came from
    command: Command
    source: Optional[Source]  # None for a command that is not a compile of one file
    directory: Path
    path: Optional[Path]  # absolute TU path; None when source is None
    name: str  # "<stem>.<hash8>": unique per output, used for every file name we create
    driver_local: str
    driver_remote: str
    object_path: Optional[Path]  # where the object ends up; None when no object is produced
    reason: str = ""  # non-empty: this unit runs locally, and this is why

    @property
    def remote(self) -> bool:
        return not self.reason

    @property
    def label(self) -> str:
        if self.source is not None:
            return self.source.text
        return self.command.sources[0].text if self.command.sources else "(command)"

    @property
    def object_name(self) -> str:
        return self.name + ".o"

    @property
    def depfile_name(self) -> str:
        return self.name + ".d"

    def local_argv(self) -> list[str]:
        argv = self.command.argv_for(self.source)
        argv[0] = self.driver_local
        return argv

    def remote_argv(self, root: Path) -> list[str]:
        """The compile on a donor, in this unit's directory inside the uploaded copy of ``root``:
        the object and the depfile land next to it under their unique names."""
        assert self.source is not None
        return self.command.compile_argv(self.source, self.driver_remote, self.object_name,
                                         depfile=self.depfile_name, root=str(root), cwd=str(self.directory))

    def working_dir_under(self, root: Path) -> str:
        """The unit's directory relative to the uploaded tree root.

        Raises ``PlanError`` when ``directory`` is outside ``root`` (donors reject ``..``).
        """
        root_r = root.resolve()
        dir_r = self.directory.resolve()
        try:
            rel = os.path.relpath(dir_r, root_r)
        except ValueError as e:  # different drives on Windows
            raise PlanError(f"unit cwd {dir_r} is outside --root {root_r}") from e
        if rel == ".":
            return "."
        if rel.startswith(".." + os.sep) or rel == "..":
            raise PlanError(f"unit cwd {dir_r} is outside --root {root_r}")
        return rel

    def natural_object(self) -> Optional[Path]:
        """Where the local compiler writes the object if we do not redirect it."""
        if self.path is None:
            return None
        if self.command.output is not None:
            out = Path(self.command.output)
            return out if out.is_absolute() else self.directory / out
        return self.directory / (self.path.stem + ".o")


@dataclass(frozen=True)
class Plan:
    units: tuple[Unit, ...]
    out_dir: Path

    @property
    def remote_units(self) -> list[Unit]:
        return [u for u in self.units if u.remote]

    @property
    def local_units(self) -> list[Unit]:
        return [u for u in self.units if not u.remote]


def infer_workspace_root(units: Sequence[Unit], explicit: Optional[Path] = None) -> Path:
    """The directory uploaded as the workspace every remote compile shares.

    Defaults to the common ancestor of every remote unit's directory, its source and the relative
    paths its preprocessor options name (``-I../include`` is project code; an absolute path may be
    a system directory, and stays the donor's own).
    """
    if explicit is not None:
        root = explicit.expanduser().resolve()
        if not root.is_dir():
            raise PlanError(f"--root {root} is not a directory")
        return root
    remote = [u for u in units if u.remote and u.directory is not None]
    if not remote:
        raise PlanError("no remote units to infer --root from; pass --root PATH")
    paths = [u.directory.resolve() for u in remote]
    for u in remote:
        if u.path is not None:
            paths.append(u.path.resolve().parent)
        paths.extend((u.directory / v).resolve() for v in u.command.path_values() if not os.path.isabs(v))
    try:
        return Path(os.path.commonpath([str(p) for p in paths]))
    except ValueError as e:
        raise PlanError(
            "remote units do not share a common directory; pass --root PATH"
        ) from e


def resolve_local(driver: str, which: Which = shutil.which) -> str:
    """The compiler binary used on this machine."""
    if os.path.isabs(driver):
        return driver
    return which(driver) or driver


def resolve_remote(driver: str, which: Which = shutil.which) -> str:
    """Absolute compiler path on the donor. The bare name is not on PATH inside the sandbox."""
    if os.path.isabs(driver):
        return driver
    return which(driver) or f"/usr/bin/{driver}"


def load_compdb(path: Path) -> list[Entry]:
    """Read a compile_commands.json. Accepts both ``command`` and ``arguments``."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise PlanError(f"cannot read compilation database {path}: {e}") from e
    if not isinstance(data, list):
        raise PlanError(f"{path}: expected a JSON list of compile entries")
    base = path.resolve().parent
    entries: list[Entry] = []
    for i, item in enumerate(data):
        if not isinstance(item, dict) or "file" not in item:
            raise PlanError(f"{path}: entry {i} has no 'file'")
        if "arguments" in item:
            argv = [str(a) for a in item["arguments"]]
        elif "command" in item:
            argv = shlex.split(str(item["command"]))
        else:
            raise PlanError(f"{path}: entry {i} has neither 'command' nor 'arguments'")
        directory = Path(str(item.get("directory", ".")))
        if not directory.is_absolute():
            directory = base / directory
        entries.append(Entry(directory=Path(os.path.normpath(directory)), argv=tuple(argv)))
    return entries


def entries_from_files(cc: str, flags: str, files: Sequence[str], cwd: Path) -> list[Entry]:
    """Form 2: every file compiled with the same ``cc`` and flag string."""
    extra = shlex.split(flags)
    return [Entry(cwd, tuple([cc, *extra, "-c", f])) for f in files]


def entries_from_command(argv: Sequence[str], cwd: Path) -> list[Entry]:
    """Form 3: one command line, as typed after ``--``."""
    return [Entry(cwd, tuple(argv))]


def _abs(directory: Path, text: str) -> Path:
    return Path(os.path.normpath(os.path.join(directory, text)))


def build_plan(
    entries: Sequence[Entry],
    out_dir: Path,
    *,
    honour_output: bool = False,
    which: Which = shutil.which,
) -> Plan:
    """Expand entries into units.

    ``honour_output`` writes a single-TU command's object to its own ``-o`` path
    instead of ``out_dir``. The CLI turns it on for the ``--`` form when ``--out``
    was not given.
    """
    units: list[Unit] = []
    seen: dict[str, tuple[str, ...]] = {}
    for entry_index, entry in enumerate(entries):
        argv = expand_response_files(list(entry.argv), entry.directory)
        if not argv:
            raise PlanError("empty compiler command")
        cmd = parse(argv)
        targets: list[Optional[Source]] = list(cmd.sources) if cmd.sources and cmd.compile_only else [None]
        for src in targets:
            reasons = cmd.reasons_for(src) if src is not None else (list(cmd.blockers) or ["not a source compile"])
            path = _abs(entry.directory, src.text) if src is not None else None
            key = f"{path}\0{cmd.output or ''}" if path is not None else " ".join(argv)
            digest = hashlib.sha256(key.encode("utf-8")).hexdigest()[:8]
            name = f"{path.stem if path is not None else 'command'}.{digest}"
            if name in seen:
                if seen[name] == tuple(argv):
                    continue  # the same compile listed twice
                raise PlanError(f"two different commands would write {name}.o")
            seen[name] = tuple(argv)

            object_path: Optional[Path] = None
            if src is not None and cmd.produces_object:
                if honour_output and cmd.output is not None and len(cmd.sources) == 1:
                    object_path = Path(os.path.normpath(os.path.join(entry.directory, cmd.output)))
                else:
                    object_path = out_dir / f"{name}.o"

            units.append(
                Unit(
                    index=len(units),
                    entry=entry_index,
                    command=cmd,
                    source=src,
                    directory=entry.directory,
                    path=path,
                    name=name,
                    driver_local=resolve_local(cmd.driver, which),
                    driver_remote=resolve_remote(cmd.driver, which),
                    object_path=object_path,
                    reason="; ".join(reasons),
                )
            )
    return Plan(units=tuple(units), out_dir=out_dir)


def describe(plan: Plan, root: Optional[Path]) -> str:
    """The plan for --dry-run: what compiles where, and with which command."""
    lines = [f"zm-cc: {len(plan.units)} unit(s): {len(plan.remote_units)} on the mesh, {len(plan.local_units)} here"]
    if root is not None:
        lines.append(f"  workspace: {root}")
    for u in plan.units:
        target = str(u.object_path) if u.object_path is not None else "-"
        if u.remote:
            lines.append(f"  mesh   {u.label}  ->  {target}")
            lines.append(f"      in {u.working_dir_under(root)}: {shlex.join(u.remote_argv(root))}")
        else:
            lines.append(f"  here   {u.label}  ->  {target}  ({u.reason})")
            lines.append(f"      in {u.directory}: {shlex.join(u.local_argv())}")
    return "\n".join(lines)


__all__ = [
    "Entry",
    "Plan",
    "PlanError",
    "Unit",
    "build_plan",
    "describe",
    "entries_from_command",
    "entries_from_files",
    "infer_workspace_root",
    "load_compdb",
    "resolve_local",
    "resolve_remote",
]
