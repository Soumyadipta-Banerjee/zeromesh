"""Real compilers, no daemon: the split path end to end on this machine.

A loopback client plays the donor. It copies each staged input tree into a fresh
directory and runs the task argv there, which is what a donor does with the
input_root. So the local ``-E`` and the remote codegen-only compile both run for
real. Skipped when the compiler is not installed.
"""

import io
import shutil
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace as NS

import pytest

from zm_cc.compile import BuildOptions, run_build
from zm_cc.plan import build_plan, entries_from_files

DATA = Path(__file__).resolve().parent / "data"

PB = NS(
    RUNNING=3,
    PLACED=2,
    SUCCEEDED=4,
    FAILED=5,
    Policy=lambda **kw: NS(**kw),
    TaskSpec=lambda **kw: NS(**kw),
    EnvSpec=lambda **kw: NS(**kw),
    Resources=lambda **kw: NS(**kw),
    JobSpec=lambda name, tasks: NS(name=name, tasks=tasks),
)


class Loopback:
    """A daemon stand-in whose donor is this machine."""

    def __init__(self, scratch: Path):
        self.scratch = scratch

    def upload(self, path, timeout=60.0):
        return Path(path)

    def submit(self, spec, timeout=10.0):
        return LoopJob(spec, self.scratch)

    def peers(self, timeout=10.0):
        return []


class LoopJob:
    job_id = "loop-1"

    def __init__(self, spec, scratch: Path):
        self.results = {}
        self.workdirs = {}
        self._events = []
        for t in spec.tasks:
            work = Path(tempfile.mkdtemp(dir=scratch))
            shutil.copytree(t.input_root, work, dirs_exist_ok=True)
            proc = subprocess.run(list(t.argv), cwd=work, capture_output=True, text=True, check=False)
            res = NS(
                exit_code=proc.returncode,
                timed_out=False,
                cancelled=False,
                error="",
                stderr_head=proc.stderr.encode("utf-8"),
                executed_by="loopback",
                duration_ms=0,
                cache_hit=False,
            )
            self.results[t.name] = res
            self.workdirs[t.name] = work
            state = PB.SUCCEEDED if proc.returncode == 0 else PB.FAILED
            self._events.append(NS(task_name=t.name, state=state, detail="", result=res))

    def events(self, timeout=None):
        yield from self._events

    def wait(self, timeout=None, check=True, poll_interval=0.05):
        return NS(results=dict(self.results), done=True)

    def cancel(self, timeout=10.0):
        pass

    def fetch(self, task, dest, only_changed=False, timeout=30.0):
        out = self.workdirs[task] / "out"
        if out.is_dir():
            shutil.copytree(out, Path(dest) / "out", dirs_exist_ok=True)
        return []


def _split_build(tmp_path, cc, flags, sources):
    scratch = tmp_path / "donor"
    scratch.mkdir()
    entries = entries_from_files(cc, flags, sources, tmp_path)
    plan = build_plan(entries, tmp_path / "objs", which=shutil.which)
    records, _ = run_build(plan, BuildOptions(out_dir=tmp_path / "objs", jobs=1), Loopback(scratch),
                           common_pb2=PB, progress=io.StringIO())
    return records


@pytest.mark.skipif(shutil.which("cc") is None, reason="needs a C compiler")
def test_c_split_compile_produces_a_working_object(tmp_path):
    (tmp_path / "src").mkdir()
    shutil.copy(DATA / "src" / "hello.c", tmp_path / "src" / "hello.c")
    shutil.copytree(DATA / "include", tmp_path / "include")
    records = _split_build(tmp_path, "cc", '-O2 -std=c11 -Iinclude "-DGREETING=hi there"', ["src/hello.c"])
    rec = records[0]
    assert rec.status == "ok", rec.error
    obj = Path(rec.output).read_bytes()
    assert obj[:4] == b"\x7fELF" and b"hello_value" in obj
    # The donor saw codegen flags only: no include path, no define.
    assert "-Iinclude" not in rec.command and "-DGREETING" not in rec.command
    assert "-x cpp-output" in rec.command


@pytest.mark.skipif(shutil.which("c++") is None, reason="needs a C++ compiler")
def test_cxx_split_compile_applies_define_before_the_remote_step(tmp_path):
    (tmp_path / "util.cc").write_text(
        "#ifndef VALUE\n#error VALUE must come from -D\n#endif\n"
        "int value_of() { return VALUE; }\n",
        encoding="utf-8",
    )
    records = _split_build(tmp_path, "c++", "-O2 -std=c++17 -DVALUE=3", ["util.cc"])
    rec = records[0]
    assert rec.status == "ok", rec.error
    assert b"value_of" in Path(rec.output).read_bytes()


@pytest.mark.skipif(shutil.which("gfortran") is None, reason="needs gfortran")
def test_fortran_split_compile_with_cpp_define(tmp_path):
    (tmp_path / "kernel.F90").write_text(
        "#ifndef SCALE\n#error SCALE must come from -D\n#endif\n"
        "function scaled(x) result(y)\n  real(8) :: x, y\n"
        "  y = real(SCALE, 8) * x\nend function scaled\n",
        encoding="utf-8",
    )
    records = _split_build(tmp_path, "gfortran", "-O2 -DSCALE=2", ["kernel.F90"])
    rec = records[0]
    assert rec.status == "ok", rec.error
    assert b"scaled" in Path(rec.output).read_bytes()
