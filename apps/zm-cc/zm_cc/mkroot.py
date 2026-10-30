"""zm-cc mkroot: build the minimal root tree that ``--env mirror`` needs to run a compiler.

    zm-cc mkroot --cc g++ --out ~/zm-root-gcc

What goes into ROOTDIR, for compiling (``-c``), not linking:

  * the driver, cc1/cc1plus (f951 for gfortran), as, ld, collect2, lto-wrapper
    (asked of the driver with ``-print-prog-name``)
  * the GCC internal directory (specs, libgcc, plugin headers)
  * the include search list (``cc -v``): the headers the probe compile sees
  * the shared libraries those binaries link (``ldd``) and their loader
  * ``/bin/sh``, because multi-compile chunks run through it
  * everything a probe compile opens (``strace``), when strace is installed

Host symlinks are recreated as symlinks, so ``/lib -> usr/lib`` stays a link. Each
file's content is copied once, at its real path, with ``cp -a --reflink=auto``.
Two copies of libc under different paths would load twice, so that is avoided.

The sandbox uses the mirror's ``usr/`` and ``opt/`` only when they exist at the top
of the mirrored directory (plugins/env-mirror/env_mirror.cpp:148-164). So ROOTDIR
itself is what you mirror, and ``usr/`` and ``opt/`` are always created.
"""

from __future__ import annotations

import argparse
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Optional

PROBE_C = '#include <stdio.h>\nint main(void) { puts("hi"); return 0; }\n'
PROBE_CXX = '#include <iostream>\nint main() { std::cout << "hi\\n"; return 0; }\n'
PROBE_FORTRAN = "program probe\n  print *, 'hi'\nend program probe\n"

_PROGS = ("cc1", "cc1plus", "f951", "as", "ld", "collect2", "lto-wrapper")
_SHARED_PREFIXES = ("/usr/", "/lib", "/bin/", "/sbin/", "/opt/", "/etc/ld.so")
_STD_LINKS = ("/lib", "/lib64", "/bin", "/sbin", "/usr/lib64")  # recreated when the host has them as links
_LDD = re.compile(r"(?:=>\s+)?(/\S+)\s+\(0x[0-9a-fA-F]+\)")
_STRACE = re.compile(r'(?:openat\(AT_FDCWD, |execve\()"([^"]+)".*\)\s*=\s*(-?\d+)')
_CP_BATCH = 200


class MkrootError(RuntimeError):
    pass


@dataclass
class Summary:
    driver: str
    root: Path
    copied: int = 0
    links: int = 0
    library_dirs: list[str] = field(default_factory=list)


def probe_for(driver: str) -> tuple[str, str]:
    """(file name, source) of a hello-world that exercises the driver's headers."""
    base = Path(driver).name
    if base.startswith("gfortran"):
        return "probe.f90", PROBE_FORTRAN
    if "++" in base:
        return "probe.cc", PROBE_CXX
    return "probe.c", PROBE_C


def _run(argv: list[str], cwd: Optional[Path] = None, stdin: Optional[str] = None) -> subprocess.CompletedProcess:
    return subprocess.run(argv, cwd=cwd, input=stdin, capture_output=True, text=True, check=False)


def query(driver: str, *args: str) -> str:
    proc = _run([driver, *args])
    return proc.stdout.strip() if proc.returncode == 0 else ""


def parse_ldd(text: str) -> set[str]:
    return {m.group(1) for m in _LDD.finditer(text)}


def parse_include_dirs(text: str) -> list[str]:
    """The ``#include <...> search starts here:`` list from ``cc -v`` (stderr)."""
    out: list[str] = []
    active = False
    for line in text.splitlines():
        if line.startswith("#include <...> search starts here:"):
            active = True
        elif line.startswith("End of search list."):
            break
        elif active and line.startswith(" "):
            out.append(line.strip().removesuffix(" (framework directory)"))
    return out


def parse_strace(text: str) -> set[str]:
    """Successful openat/execve paths from an ``strace -qq -e trace=openat,execve`` log."""
    paths: set[str] = set()
    for line in text.splitlines():
        m = _STRACE.search(line)
        if m and not m.group(2).startswith("-"):
            paths.add(m.group(1))
    return {p for p in paths if p.startswith(_SHARED_PREFIXES)}


def ldd_closure(binary: str) -> set[str]:
    if not os.path.isfile(binary):
        return set()
    return parse_ldd(_run(["ldd", binary]).stdout)


def collect_strace(driver: str, workdir: Path) -> set[str]:
    """Compile a probe under strace and record what it opened. Empty when strace is absent."""
    strace = shutil.which("strace")
    if strace is None:
        return set()
    name, text = probe_for(driver)
    (workdir / name).write_text(text, encoding="utf-8")
    log = workdir / "strace.log"
    proc = _run(
        [strace, "-f", "-qq", "-e", "trace=openat,execve", "-o", str(log),
         driver, "-c", name, "-o", "probe.o"],
        cwd=workdir,
    )
    if not (workdir / "probe.o").is_file():
        raise MkrootError(f"probe compile failed under strace: {proc.stderr.strip()[-400:]}")
    return parse_strace(log.read_text(encoding="utf-8", errors="replace"))


def collect_tools(driver: str, cxx: bool) -> tuple[set[str], set[str]]:
    """Paths the compiler needs, from ``-print-prog-name`` and ``-v``. Returns (paths, libraries)."""
    binaries = {driver}
    for prog in _PROGS:
        p = query(driver, f"-print-prog-name={prog}")
        if os.path.isabs(p):
            binaries.add(p)
    paths = set(binaries) | {"/bin/sh"}
    libgcc = query(driver, "-print-libgcc-file-name")
    if os.path.isabs(libgcc):
        paths.add(os.path.dirname(libgcc))
    if not Path(driver).name.startswith("gfortran"):
        proc = _run([driver, "-E", "-x", "c++" if cxx else "c", "-v", "-"], stdin="")
        paths |= set(parse_include_dirs(proc.stderr))
    libs: set[str] = set()
    for b in binaries | {"/bin/sh"}:
        libs |= ldd_closure(b)
    return paths | libs, libs


class Stager:
    """Copy host paths into a root tree. Symlinks stay symlinks; content is copied once."""

    def __init__(self, root: Path, host: Path = Path("/")) -> None:
        self.root = root
        self.host = Path(os.path.realpath(host))
        self.missing: list[str] = []
        self._links: dict[str, str] = {}  # logical path -> link target; created after the copies
        self._links_done: set[str] = set()
        self._copies: set[str] = set()

    def _phys(self, logical: str) -> Path:
        return self.host / logical.lstrip("/")

    def _replicate_links(self, logical: str) -> None:
        """Record each symlink on ``logical`` and on the chain it points to. No content copied."""
        if logical in self._links_done:
            return
        self._links_done.add(logical)
        cur = "/"
        for part in PurePosixPath(logical).parts[1:]:
            cur = posixpath.join(cur, part)
            phys = self._phys(cur)
            if phys.is_symlink():
                target = os.readlink(phys)
                self._links.setdefault(cur, target)
                self._replicate_links(posixpath.normpath(posixpath.join(posixpath.dirname(cur), target)))

    def add(self, logical: str) -> None:
        """Stage one absolute host path: its symlinks, then the real file or directory it resolves to."""
        if not os.path.lexists(self._phys(logical)):
            self.missing.append(logical)
            return
        self._replicate_links(logical)
        real = Path(os.path.realpath(self._phys(logical)))
        try:
            rel = real.relative_to(self.host)
        except ValueError:
            return  # resolves outside the host tree (absolute link into another mount): skip
        self._copies.add("/" + rel.as_posix())

    def add_standard_links(self) -> None:
        for logical in _STD_LINKS:
            if self._phys(logical).is_symlink():
                self._replicate_links(logical)

    def flush(self) -> tuple[int, int]:
        """Copy content first, then create the symlinks. Returns (top-level copies, symlinks).

        Content first, because a directory link such as ``lib -> usr/lib`` may be created
        only once its target exists. Shallow links go first, so ``bin`` is a link before ``bin/sh``.
        """
        keep = sorted(p for p in self._copies if not any(p.startswith(q + "/") for q in self._copies))
        rels = [p.lstrip("/") for p in keep]
        for i in range(0, len(rels), _CP_BATCH):
            batch = rels[i : i + _CP_BATCH]
            proc = _run(["cp", "-a", "--reflink=auto", "--parents", "-t", str(self.root), *batch], cwd=self.host)
            if proc.returncode != 0:
                raise MkrootError(f"cp failed: {proc.stderr.strip()[-400:]}")
        links = 0
        for logical in sorted(self._links, key=lambda p: p.count("/")):
            dest = self.root / logical.lstrip("/")
            if os.path.lexists(dest):
                continue  # already copied as a real entry
            dest.parent.mkdir(parents=True, exist_ok=True)
            os.symlink(self._links[logical], dest)
            links += 1
        return len(keep), links


def build_root(driver_name: str, out: Path, *, use_strace: bool = True, host: Path = Path("/")) -> Summary:
    driver = driver_name if os.path.isabs(driver_name) else shutil.which(driver_name)
    if not driver:
        raise MkrootError(f"compiler not found: {driver_name}")
    if out.exists() and any(out.iterdir()):
        raise MkrootError(f"{out} exists and is not empty")
    out.mkdir(parents=True, exist_ok=True)
    (out / "usr").mkdir(exist_ok=True)
    (out / "opt").mkdir(exist_ok=True)

    cxx = "++" in Path(driver).name
    with tempfile.TemporaryDirectory(prefix="zm-cc-mkroot-") as td:
        paths, libs = collect_tools(driver, cxx)
        if use_strace:
            paths |= collect_strace(driver, Path(td))

    stager = Stager(out, host)
    for p in sorted(paths):
        stager.add(p)
    stager.add_standard_links()
    copied, links = stager.flush()

    if not os.path.exists(out / driver.lstrip("/")):
        raise MkrootError(f"{driver} did not land in {out}; cannot compile with this root")
    lib_dirs = sorted({os.path.dirname(p) for p in libs if not p.startswith(("/usr/lib", "/lib"))})
    return Summary(driver=driver, root=out, copied=copied, links=links, library_dirs=lib_dirs)


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(prog="zm-cc mkroot", description="Build a minimal compiler root for --env mirror.")
    p.add_argument("--cc", required=True, help="compiler name or absolute path, e.g. g++ or /usr/bin/g++")
    p.add_argument("--out", required=True, type=Path, help="root directory to create (absent or empty)")
    p.add_argument("--no-strace", action="store_true", help="skip the strace probe even when strace is installed")
    args = p.parse_args(argv)
    try:
        s = build_root(args.cc, args.out, use_strace=not args.no_strace)
    except (MkrootError, OSError, subprocess.SubprocessError) as e:
        print(f"zm-cc mkroot: {e}", file=sys.stderr)
        return 1
    root = s.root.resolve()
    print(f"zm-cc mkroot: {s.driver} -> {root} ({s.copied} tree(s) copied, {s.links} symlink(s))", file=sys.stderr)
    print(f"  mirror it with:  zm-cc ... --env mirror --mirror {root}", file=sys.stderr)
    if s.library_dirs:
        print(
            "  libraries outside /usr/lib are used; add:  --ld-library-path "
            + ":".join(s.library_dirs),
            file=sys.stderr,
        )
    return 0
