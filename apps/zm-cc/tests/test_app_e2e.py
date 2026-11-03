"""zm-cc against a real zmd and the real compilers."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
for p in (str(REPO / "apps" / "zm-cc"), str(REPO / "sdk" / "python"), str(REPO / "tests" / "integration")):
    if p not in sys.path:
        sys.path.insert(0, p)

import zmtest  # noqa: E402
from zeromesh import report  # noqa: E402
from zeromesh.v1 import run_pb2  # noqa: E402
from zm_cc.cli import main  # noqa: E402

pytestmark = pytest.mark.skipif(not (shutil.which("cc") and shutil.which("c++")), reason="needs cc and c++")


@pytest.fixture(scope="module")
def zmd():
    daemon = zmtest.Zmd()
    yield daemon
    daemon.close()


def project(tmp: Path) -> Path:
    """A CMake-like layout: commands run in build/, sources in src/, headers in include/."""
    root = tmp / "proj"
    for d in ("src", "include", "build"):
        (root / d).mkdir(parents=True)
    (root / "include" / "add.h").write_text("int add(int a, int b);\n")
    (root / "include" / "twice.h").write_text("#define TWICE(x) (2 * (x))\n")
    (root / "src" / "add.c").write_text('#include "add.h"\nint add(int a, int b) { return a + b; }\n')
    (root / "src" / "twice.c").write_text('#include "twice.h"\nint twice(int x) { return TWICE(x); }\n')
    (root / "src" / "main.cpp").write_text(
        '#include <cstdio>\nextern "C" int add(int, int);\nextern "C" int twice(int);\n'
        'int main() { std::printf("%d\\n", twice(add(GREETING, 1))); return 0; }\n')
    (root / "build" / "stale.o").write_bytes(b"\0" * 4096)  # a build product: never uploaded
    entries = [
        {"directory": str(root / "build"), "file": "../src/add.c",
         "command": "cc -O2 -I../include -MD -MF add.d -c ../src/add.c -o add.o", "output": "add.o"},
        {"directory": str(root / "build"), "file": "../src/twice.c",
         "command": "cc -O2 -I../include -c ../src/twice.c -o twice.o", "output": "twice.o"},
        {"directory": str(root / "build"), "file": "../src/main.cpp",
         "command": "c++ -O2 -DGREETING=20 -c ../src/main.cpp -o main.o", "output": "main.o"},
    ]
    (root / "build" / "compile_commands.json").write_text(json.dumps(entries))
    return root


def zmcc(zmd, *argv: str) -> int:
    with pytest.raises(SystemExit) as e:
        main(["--socket", zmd.socket, *argv])
    return e.value.code


def test_compiles_links_caches_and_recompiles_only_what_a_header_edit_touches(zmd, tmp_path):
    root = project(tmp_path)
    out, prog = tmp_path / "objs", tmp_path / "prog"
    compdb = str(root / "build" / "compile_commands.json")
    assert zmcc(zmd, "--compdb", compdb, "-o", str(out), "--link-output", str(prog)) == 0, zmd.log()
    assert subprocess.run([str(prog)], capture_output=True, text=True).stdout == "42\n"
    rep = report.load(str(out / "report.json"))
    assert rep.run.succeeded == 3 and rep.run.cached == 0
    # The cache keys on what each compile read (its depfile), not on the whole tree.
    assert any(f.endswith("include/add.h") for r in rep.items for f in r.footprint)

    assert zmcc(zmd, "--compdb", compdb, "-o", str(out)) == 0
    assert report.load(str(out / "report.json")).run.cached == 3

    (root / "include" / "twice.h").write_text("#define TWICE(x) (3 * (x))\n")
    assert zmcc(zmd, "--compdb", compdb, "-o", str(out), "--link-output", str(prog)) == 0
    rep = report.load(str(out / "report.json"))
    hits = {r.id.split(".")[0]: r.cache.hit for r in rep.items}
    assert hits == {"add": True, "twice": False, "main": True}
    assert subprocess.run([str(prog)], capture_output=True, text=True).stdout == "63\n"


def test_a_compile_error_fails_its_unit_and_skips_the_link(zmd, tmp_path):
    root = project(tmp_path)
    (root / "src" / "twice.c").write_text("int twice(int x) { return x +; }\n")
    out = tmp_path / "objs"
    rc = zmcc(zmd, "--compdb", str(root / "build" / "compile_commands.json"), "-o", str(out), "--keep-going",
              "--link-output", str(tmp_path / "prog"))
    assert rc == 1
    failed = report.failed(report.load(str(out / "report.json")))
    assert [(r.id.split(".")[0], r.failure.kind) for r in failed] == [("twice", run_pb2.FAILURE_EXIT)]
    assert b"error" in failed[0].failure.stderr_tail
    assert not (tmp_path / "prog").exists()


def test_a_command_that_cannot_be_split_runs_here(zmd, tmp_path, monkeypatch):
    (tmp_path / "hello.c").write_text('#include <stdio.h>\nint main(void) { puts("hi"); return 0; }\n')
    monkeypatch.chdir(tmp_path)
    assert zmcc(zmd, "--", "cc", "hello.c", "-o", "hello") == 0  # compile and link in one: local
    assert subprocess.run(["./hello"], capture_output=True, text=True).stdout == "hi\n"


@pytest.mark.parametrize("compiler, name, source, flags, expect", [
    ("gfortran", "show.F90", "program show\n  print '(I0)', N * 2\nend program show\n", "-O2 -DN=21", "42\n"),
    ("clang", "hello.c", '#include <stdio.h>\nint main(void) { printf("%d\\n", N); return 0; }\n', "-O2 -DN=42",
     "42\n"),
])
def test_other_compilers_build_through_the_mesh(zmd, tmp_path, monkeypatch, compiler, name, source, flags, expect):
    if not shutil.which(compiler):
        pytest.skip(f"needs {compiler}")
    (tmp_path / name).write_text(source)
    monkeypatch.chdir(tmp_path)
    prog = tmp_path / "prog"
    assert zmcc(zmd, "--cc", compiler, "--flags", flags, name, "-o", str(tmp_path / "objs"), "--link-output",
                str(prog)) == 0, zmd.log()
    assert report.load(str(tmp_path / "objs" / "report.json")).run.succeeded == 1  # compiled on the mesh
    assert subprocess.run([str(prog)], capture_output=True, text=True).stdout.strip() == expect.strip()


@pytest.mark.capture  # the first capture hashes the compiler's whole header search list: slow, run on its own
@pytest.mark.skipif(not shutil.which("strace"), reason="needs strace")
def test_compiles_inside_a_captured_toolchain(zmd, tmp_path):
    root = project(tmp_path)
    out = tmp_path / "objs"
    assert zmcc(zmd, "--compdb", str(root / "build" / "compile_commands.json"), "-o", str(out), "--env",
                "capture", "--no-cache", "--link-output", str(tmp_path / "prog")) == 0, zmd.log()
    assert subprocess.run([str(tmp_path / "prog")], capture_output=True, text=True).stdout == "42\n"
