"""zm-cc: compile across a ZeroMesh team, one item per translation unit.

Three input forms, exactly one per invocation:

  zm-cc --compdb build/compile_commands.json
  zm-cc --cc g++ --flags "-O2 -std=c++20 -Iinclude" src/a.cpp src/b.cpp
  zm-cc -- gcc -O2 -c foo.c -o foo.o

Everything after the first ``--`` is the command; everything before it is zm-cc's options.
"""

from __future__ import annotations

import os
import shlex
import sys
from pathlib import Path
from typing import Optional, Sequence

from zeromesh.adapter import Adapter, AdapterError, Depfile, Item, Out, Res, Workspace, run_adapter

from zm_cc import local, toolchain
from zm_cc.plan import (PlanError, build_plan, describe, entries_from_command, entries_from_files,
                        infer_workspace_root, load_compdb)

# Build products never travel: the compiles do not read them, and a build directory inside the
# workspace would otherwise ship every object of the last build.
BUILD_PRODUCTS = ["*.o", "*.obj", "*.a", "*.so", "*.so.*", "*.d", "*.gch", "*.pch", "*.gcda", "*.gcno",
                  ".ninja_deps", ".ninja_log", ".cache/", "__pycache__/", "zm-out/", "zm-objs/"]


def binaries_under(root: Path, dirs: Sequence[Path]) -> list[str]:
    """Anchored exclude patterns for the ELF files (programs, libraries, objects) in `dirs` and below:
    a build directory's executables have no telling extension, and no compile reads one."""
    root = root.resolve()
    tops = sorted({d.resolve() for d in dirs})
    tops = [d for d in tops if not any(t != d and t in d.parents for t in tops)]
    found = []
    for top in tops:
        for dirpath, dirnames, files in os.walk(top):
            dirnames[:] = [d for d in dirnames if d != ".git"]
            for name in files:
                path = os.path.join(dirpath, name)
                if os.path.islink(path) or any(c in name for c in "*?[]!#\\"):
                    continue
                try:
                    with open(path, "rb") as f:
                        if f.read(4) != b"\x7fELF":
                            continue
                except OSError:
                    continue
                found.append("/" + os.path.relpath(path, root))
    return found


class Cc(Adapter):
    name = "zm-cc"
    kind = "cc"
    res = Res(cores=1)  # a compiler is one thread; raise --cores for -flto=auto

    def __init__(self, command: Optional[list[str]] = None, out_given: bool = False):
        self.command = command        # what followed "--"
        self.out_given = out_given    # -o/--out was on the command line

    def add_args(self, p):
        p.add_argument("sources", nargs="*", help="source files, compiled with --cc and --flags")
        p.add_argument("--compdb", type=Path, metavar="FILE", help="compile_commands.json")
        p.add_argument("--cc", default="cc", help="compiler for SOURCES (default: cc)")
        p.add_argument("--flags", default="", help='flags for SOURCES, one string, e.g. "-O2 -Iinclude"')
        p.add_argument("--root", type=Path, metavar="DIR", help="workspace to upload (default: the common "
                                                                "directory of every unit and source)")
        p.add_argument("--link-output", type=Path, metavar="PATH", help="link the objects into PATH here")
        p.add_argument("--link-flags", default="", metavar="FLAGS", help="extra flags for --link-output")
        p.add_argument("--link-ninja", type=Path, metavar="BUILD_DIR",
                       help="with --compdb: run the ninja link steps of --link-targets from these objects")
        p.add_argument("--link-targets", nargs="+", default=[], metavar="TARGET")

    def check(self, a):
        forms = [a.compdb is not None, bool(a.sources), self.command is not None]
        if sum(forms) != 1:
            raise AdapterError("give exactly one of --compdb FILE, SOURCES..., or -- COMMAND")
        if self.command is not None and not self.command:
            raise AdapterError("nothing after --")
        cwd = Path.cwd()
        try:
            if a.compdb is not None:
                entries = load_compdb(a.compdb)
            elif a.sources:
                entries = entries_from_files(a.cc, a.flags, a.sources, cwd)
            else:
                entries = entries_from_command(self.command, cwd)
            # `-- cc -c x.c -o x.o` without --out writes x.o where the command says.
            self.tus = build_plan(entries, Path(a.out).absolute(),
                                   honour_output=self.command is not None and not self.out_given)
            if not self.tus.units:
                raise AdapterError("no translation units to build")
            self.root = None
            if self.tus.remote_units:
                self.root = infer_workspace_root(self.tus.remote_units, a.root)
                for u in self.tus.remote_units:
                    u.working_dir_under(self.root)  # a unit outside the workspace is an error, not a surprise
        except PlanError as e:
            raise AdapterError(str(e)) from None
        if a.link_ninja is not None and (a.compdb is None or not a.link_targets):
            raise AdapterError("--link-ninja needs --compdb and --link-targets")
        remote = self.tus.remote_units
        if a.env == "capture" and remote:
            self.env = toolchain.capture_env(remote, self.root)
        elif remote:
            self.env = toolchain.host_env(remote)

    def workspace(self, a):
        root = self.root or Path.cwd()
        dirs = [u.directory for u in self.tus.remote_units]
        return Workspace(str(root), exclude=BUILD_PRODUCTS + binaries_under(root, dirs))

    def items(self, a):
        for u in self.tus.remote_units:
            wd = u.working_dir_under(self.root)
            yield Item(id=u.name.replace(" ", "_"), argv=u.remote_argv(self.root), cwd="" if wd == "." else wd,
                       outputs=[Out(u.object_name, dest=str(u.object_path))], footprint=Depfile(u.depfile_name),
                       cost=float(u.path.stat().st_size) if u.path and u.path.exists() else 0.0)

    def plan(self, a):
        return describe(self.tus, self.root)

    def finish(self, a, run):
        rc = 0
        if self.tus.local_units:
            for r in local.run_units(self.tus.local_units):
                mark = "here    " if r.ok else "FAILED  "
                print(f"  {mark}{r.unit.label}" + (f": {r.error}" if r.error else f"  ({r.unit.reason})"),
                      file=sys.stderr)
                rc = rc or (0 if r.ok else 1)
        if rc or run.failed:
            if a.link_output or a.link_ninja:
                print("zm-cc: not linking: some units failed", file=sys.stderr)
            return 1
        compiled = [u for u in self.tus.units if u.object_path is not None]
        if a.link_output is not None:
            objects = [str(u.object_path) for u in compiled]
            code, err = local.link(local.link_driver(self.tus.units), objects, a.link_output,
                                   shlex.split(a.link_flags))
            if code != 0:
                print(f"zm-cc: link failed: {err.strip()[-400:]}", file=sys.stderr)
                return 1
            print(f"zm-cc: linked {len(objects)} object(s) into {a.link_output}", file=sys.stderr)
        if a.link_ninja is not None:
            from zm_cc.ninja_link import link_build

            code, err, used = link_build(a.link_ninja, a.compdb, compiled, a.link_targets)
            if code != 0:
                print(f"zm-cc: ninja link failed: {err.strip()[-400:]}", file=sys.stderr)
                return 1
            print(f"zm-cc: linked {', '.join(a.link_targets)} from {used} zm-cc object(s)", file=sys.stderr)
        return 0


def main(argv: Optional[Sequence[str]] = None) -> None:
    raw = list(sys.argv[1:] if argv is None else argv)
    command = None
    if "--" in raw:
        i = raw.index("--")
        raw, command = raw[:i], raw[i + 1:]
    out_given = any(x in ("-o", "--out") or x.startswith("--out=") for x in raw)
    sys.exit(run_adapter(Cc(command, out_given), raw))


if __name__ == "__main__":
    main()
