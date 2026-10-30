"""Build orchestration against a fake daemon and a fake local runner. No zmd, no donors, no compiler."""

import io
import os
import re
from pathlib import Path
from types import SimpleNamespace as NS

from zm_cc.compile import BuildOptions, build_job_spec, link_objects, run_build
from zm_cc.plan import build_plan, entries_from_command, entries_from_files

NODE = "abcdef1234567890"

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


def fake_which(name):
    return f"/usr/bin/{name}"


class FakeJob:
    job_id = "job-1"

    def __init__(self, client, spec):
        self.client = client
        self.spec = spec
        self.cancelled = False
        self._results = {}
        self._produced = {}
        self._events = []
        for t in spec.tasks:
            outs = re.findall(r"out/([\w.+-]+\.o)", " ".join(t.argv))
            code, produced = client.behaviour(t.name, outs)
            res = NS(
                exit_code=code,
                timed_out=False,
                cancelled=False,
                error="",
                stderr_head=b"boom" if code else b"",
                executed_by=NODE,
                duration_ms=50,
                cache_hit=False,
            )
            self._results[t.name] = res
            self._produced[t.name] = produced
            self._events.append(NS(task_name=t.name, state=PB.RUNNING, detail="", result=None))
            self._events.append(
                NS(task_name=t.name, state=PB.SUCCEEDED if code == 0 else PB.FAILED, detail="", result=res)
            )

    def events(self, timeout=None):
        yield from self._events

    def wait(self, timeout=None, check=True, poll_interval=0.05):
        return NS(results=dict(self._results), done=True)

    def cancel(self, timeout=10.0):
        self.cancelled = True

    def fetch(self, task, dest, only_changed=False, timeout=30.0):
        written = []
        for n in self._produced[task]:
            p = Path(dest) / "out" / n
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b"\x7fELF-fake " + n.encode())
            written.append(str(p))
        return written


class FakeClient:
    """behaviour(task, object_names) -> (exit code, names the task actually wrote)."""

    def __init__(self, *, fail=(), partial=(), silent=()):
        self.fail, self.partial, self.silent = set(fail), set(partial), set(silent)
        self.uploads: list[list[str]] = []
        self.jobs: list[FakeJob] = []
        self.timeouts: list[float] = []

    def behaviour(self, task, names):
        if task in self.fail:
            return 1, []
        if task in self.partial:
            return 1, names[:1]
        if task in self.silent:
            return 0, []
        return 0, names

    def upload(self, path, timeout=60.0, **_):
        self.timeouts.append(timeout)
        files = []
        for root, _d, fs in os.walk(path):
            files += [os.path.relpath(os.path.join(root, f), path) for f in fs]
        self.uploads.append(sorted(files))
        return "root-digest"

    def submit(self, spec, timeout=10.0):
        self.timeouts.append(timeout)
        job = FakeJob(self, spec)
        self.jobs.append(job)
        return job

    def peers(self, timeout=10.0):
        return [NS(node_id=NODE, hostname="donor1")]


class FakeRunner:
    """Local runner: ``-E`` writes its -o file; anything else writes an object."""

    def __init__(self):
        self.calls: list[tuple[list[str], Path]] = []

    def __call__(self, argv, cwd):
        self.calls.append((list(argv), Path(cwd)))
        joined = " ".join(argv)
        if "-E" in argv:
            if "BROKEN" in joined:
                return 1, "error: BROKEN source"
            Path(argv[argv.index("-o") + 1]).write_text("# preprocessed\n", encoding="utf-8")
            return 0, ""
        if "-o" in argv:
            target = Path(argv[argv.index("-o") + 1])
        else:  # compilers name the object after the source, in the working directory
            src = next(a for a in reversed(argv) if not a.startswith("-"))
            target = Path(src).with_suffix(".o")
        if not target.is_absolute():
            target = Path(cwd) / target
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"local-object")
        return 0, ""


def _opts(tmp_path, **kw):
    return BuildOptions(out_dir=tmp_path / "objs", jobs=2, **kw)


def _plan(tmp_path, files=("a.c", "b.c", "c.c"), flags="-O2 -Iinc -DX=1"):
    entries = entries_from_files("cc", flags, list(files), tmp_path)
    return build_plan(entries, tmp_path / "objs", which=fake_which)


def test_success_places_objects_and_records_nodes(tmp_path):
    plan = _plan(tmp_path)
    client = FakeClient()
    records, total = run_build(plan, _opts(tmp_path, chunk=2), client, common_pb2=PB,
                               runner=FakeRunner(), progress=io.StringIO())
    assert [r.status for r in records] == ["ok", "ok", "ok"]
    for r in records:
        assert Path(r.output).read_bytes().startswith(b"\x7fELF")
    assert records[0].node == "donor1 (abcdef12)"
    assert (records[0].task, records[2].task) == ("cc_0000", "cc_0001")
    assert records[0].ship_bytes > 0 and total >= 0
    staged = client.uploads[0]
    assert "out/.zm-keep" in staged
    assert sum(f.endswith(".i") for f in staged) == 3  # preprocessed inputs, nothing else


def test_failed_task_cancels_job_without_keep_going(tmp_path):
    client = FakeClient(fail={"cc_0000"})
    plan = _plan(tmp_path, ("a.c", "b.c"))
    records, _ = run_build(plan, _opts(tmp_path), client, common_pb2=PB, runner=FakeRunner(),
                           progress=io.StringIO())
    assert client.jobs[0].cancelled
    assert records[0].status == "failed" and "exit 1: boom" in records[0].error


def test_keep_going_does_not_cancel(tmp_path):
    client = FakeClient(fail={"cc_0000"})
    records, _ = run_build(_plan(tmp_path, ("a.c", "b.c")), _opts(tmp_path, keep_going=True), client,
                           common_pb2=PB, runner=FakeRunner(), progress=io.StringIO())
    assert not client.jobs[0].cancelled
    assert [r.status for r in records] == ["failed", "ok"]


def test_partial_chunk_keeps_objects_that_compiled(tmp_path):
    client = FakeClient(partial={"cc_0000"})
    records, _ = run_build(_plan(tmp_path, ("a.c", "b.c")), _opts(tmp_path, chunk=2, keep_going=True),
                           client, common_pb2=PB, runner=FakeRunner(), progress=io.StringIO())
    assert records[0].status == "ok"
    assert records[1].status == "failed" and records[1].error.startswith("exit 1: boom")


def test_task_success_without_object_is_a_failure(tmp_path):
    client = FakeClient(silent={"cc_0000"})
    records, _ = run_build(_plan(tmp_path, ("a.c",)), _opts(tmp_path), client, common_pb2=PB,
                           runner=FakeRunner(), progress=io.StringIO())
    assert records[0].status == "failed" and records[0].error == "no object produced"


def test_preprocess_failure_isolated_with_keep_going(tmp_path):
    client = FakeClient()
    records, _ = run_build(_plan(tmp_path, ("a.c", "BROKEN.c", "c.c")), _opts(tmp_path, keep_going=True),
                           client, common_pb2=PB, runner=FakeRunner(), progress=io.StringIO())
    assert [r.status for r in records] == ["ok", "failed", "ok"]
    assert "BROKEN source" in records[1].error
    assert len(client.jobs[0].spec.tasks) == 2  # the broken unit was never submitted


def test_preprocess_failure_without_keep_going_submits_nothing(tmp_path):
    client = FakeClient()
    records, _ = run_build(_plan(tmp_path, ("a.c", "BROKEN.c", "c.c")), _opts(tmp_path), client,
                           common_pb2=PB, runner=FakeRunner(), progress=io.StringIO())
    assert [r.status for r in records] == ["skipped", "failed", "skipped"]
    assert client.jobs == []


def test_local_passthrough_object_moves_into_out_dir(tmp_path):
    entries = entries_from_command(["cc", "-O2", "-c", "local.c", "-fmodules-ts"], tmp_path)
    plan = build_plan(entries, tmp_path / "objs", which=fake_which)
    records, _ = run_build(plan, _opts(tmp_path), None, runner=FakeRunner(), progress=io.StringIO())
    rec = records[0]
    assert rec.status == "ok" and rec.mode == "local"
    assert Path(rec.output).parent == tmp_path / "objs"
    assert not (tmp_path / "local.o").exists()  # moved, not copied


def test_local_only_build_needs_no_client(tmp_path):
    plan = build_plan(entries_from_command(["cc", "-c", "x.c", "-fsyntax-only"], tmp_path),
                      tmp_path / "objs", which=fake_which)
    records, _ = run_build(plan, _opts(tmp_path), None, runner=FakeRunner(), progress=io.StringIO())
    assert records[0].status == "ok" and records[0].output == ""  # -fsyntax-only: no object


def test_stale_object_never_passes_for_this_run(tmp_path):
    plan = _plan(tmp_path, ("a.c",))
    stale = plan.units[0].object_path
    stale.parent.mkdir(parents=True)
    stale.write_bytes(b"old")
    records, _ = run_build(plan, _opts(tmp_path), FakeClient(fail={"cc_0000"}), common_pb2=PB,
                           runner=FakeRunner(), progress=io.StringIO())
    assert records[0].status == "failed"
    assert not stale.exists()


def test_submit_and_upload_use_the_submit_timeout(tmp_path):
    client = FakeClient()
    run_build(_plan(tmp_path, ("a.c",)), _opts(tmp_path, submit_timeout=77.0), client, common_pb2=PB,
              runner=FakeRunner(), progress=io.StringIO())
    assert client.timeouts == [77.0, 77.0]


def test_build_job_spec_fields(tmp_path):
    plan = _plan(tmp_path, ("a.c", "b.c"))
    from zm_cc.plan import make_tasks

    tasks = make_tasks(plan.remote_units, chunk=1)
    opts = BuildOptions(out_dir=tmp_path, env="mirror", mirror_paths=["/r/root"], cores=3,
                        ld_library_path="/opt/x/lib", may_run_local=False)
    spec = build_job_spec(input_root="root", tasks=tasks, opts=opts, common_pb2=PB)
    t = spec.tasks[0]
    assert spec.name == "zm-cc" and len(spec.tasks) == 2
    assert t.outputs == ["out/"] and t.resources.cores == 3
    assert t.env_spec.name == "mirror" and t.env_spec.params == {"mirror_paths": "/r/root"}
    assert t.env == {"LD_LIBRARY_PATH": "/opt/x/lib"}
    assert t.policy.may_run_local is False and t.policy.may_run_remote is True


def test_link_uses_cxx_driver_when_any_unit_is_cxx(tmp_path):
    entries = entries_from_files("cc", "-O2", ["a.c"], tmp_path) + entries_from_files(
        "c++", "-O2", ["b.cc"], tmp_path)
    plan = build_plan(entries, tmp_path / "objs", which=fake_which)
    client = FakeClient()
    records, _ = run_build(plan, _opts(tmp_path), client, common_pb2=PB, runner=FakeRunner(),
                           progress=io.StringIO())
    runner = FakeRunner()
    code, _ = link_objects(plan, records, output=tmp_path / "prog", flags="-lm", runner=runner)
    argv, _cwd = runner.calls[-1]
    assert code == 0
    assert argv[0] == "/usr/bin/c++"
    assert argv[-2:] == ["-o", str(tmp_path / "prog")] and "-lm" in argv
    assert all(Path(a).name.endswith(".o") for a in argv[1:3])
