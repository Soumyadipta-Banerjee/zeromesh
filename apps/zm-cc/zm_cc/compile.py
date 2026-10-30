"""Run a Plan: preprocess remote units locally, compile them on the mesh, place the objects.

Flow for one build:

  1. ``cc -E`` every remote unit into a staging dir (in parallel, ``--jobs``).
  2. Upload the staging dir once and submit one job. Each task is ``--chunk`` units.
  3. Passthrough (local) units run here meanwhile.
  4. Fetch each task's ``out/`` and move every object to its place in ``--out``.

An object is good when it exists and is non-empty. A task can fail after some of
its units have compiled, so placement is checked per unit, not per task.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Optional, TextIO

from zm_cc.plan import Plan, RemoteTask, Unit, make_tasks
from zm_cc.report import UnitRecord, explain, peer_names, task_failure, who

Runner = Callable[[list[str], Path], tuple[int, str]]


def run_process(argv: list[str], cwd: Path) -> tuple[int, str]:
    """Run one local command. Returns (exit code, stderr text)."""
    try:
        proc = subprocess.run(argv, cwd=str(cwd), capture_output=True, check=False)
    except OSError as e:
        return 127, str(e)
    return proc.returncode, proc.stderr.decode("utf-8", errors="replace")


@dataclass
class BuildOptions:
    out_dir: Path
    env: str = "host"  # host: donor must match our toolchain byte for byte; mirror: use our root
    mirror_paths: list[str] = field(default_factory=list)
    ld_library_path: str = ""
    # Cores per task. A compiler is mostly single-threaded, so 2 leaves room for the
    # donor's own work and for a second compile; raise it only for LTO or -flto=auto.
    cores: int = 2
    chunk: int = 1
    jobs: int = 4
    may_run_local: bool = True
    keep_going: bool = False
    submit_timeout: float = 1800.0
    verbose: bool = False


def build_job_spec(*, input_root: Any, tasks: list[RemoteTask], opts: BuildOptions, common_pb2: Any) -> Any:
    """One TaskSpec per RemoteTask. ``common_pb2`` is passed in so tests can stub it."""
    policy = common_pb2.Policy(may_run_local=opts.may_run_local, may_run_remote=True)
    params: dict[str, str] = {}
    if opts.env == "mirror" and opts.mirror_paths:
        params["mirror_paths"] = "\n".join(opts.mirror_paths)
    env = {"LD_LIBRARY_PATH": opts.ld_library_path} if opts.ld_library_path else {}
    specs = []
    for t in tasks:
        specs.append(
            common_pb2.TaskSpec(
                name=t.name,
                argv=list(t.argv),
                input_root=input_root,
                outputs=["out/"],
                env_spec=common_pb2.EnvSpec(name=opts.env, params=params),
                resources=common_pb2.Resources(cores=opts.cores),
                policy=policy,
                env=env,
            )
        )
    return common_pb2.JobSpec(name="zm-cc", tasks=specs)


def _tail(err: str, lines: int = 3) -> str:
    kept = [ln for ln in err.splitlines() if ln.strip()]
    return " | ".join(kept[-lines:])


def _event(prog: TextIO, kind: str, label: str, detail: str = "") -> None:
    line = f"{kind:9} {label:<34}"
    if detail:
        line += f"  {detail}"
    print(line, file=prog, flush=True)


def task_label(task: RemoteTask) -> str:
    first = task.units[0].label
    return f"{task.name} {first}" + (f" +{len(task.units) - 1}" if len(task.units) > 1 else "")


def _stage_unit(u: Unit, stage: Path, runner: Runner) -> tuple[int, str]:
    """Write the preprocessed input for one remote unit. Returns (bytes, error)."""
    assert u.source is not None and u.path is not None
    dst = stage / u.stage_name
    try:
        if u.source.lang is not None and u.source.lang.preprocessed:
            shutil.copyfile(u.path, dst)  # already preprocessor output: nothing to run
        else:
            code, err = runner(u.preprocess_argv(str(dst)), u.directory)
            if code != 0:
                return 0, f"preprocess exit {code}: {_tail(err)}"
    except OSError as e:
        return 0, str(e)
    if not dst.is_file() or dst.stat().st_size == 0:
        return 0, "preprocessor wrote nothing"
    return dst.stat().st_size, ""


def _fresh(path: Path, before: Optional[int]) -> bool:
    """True when ``path`` is a non-empty file that the compile just (re)wrote."""
    if not path.is_file() or path.stat().st_size == 0:
        return False
    return before is None or path.stat().st_mtime_ns != before


def _move(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.unlink()
    shutil.move(str(src), str(dst))


def _run_local(u: Unit, runner: Runner, stop: threading.Event) -> tuple[str, float, str]:
    """Run a passthrough unit unchanged. Returns (status, seconds, error)."""
    if stop.is_set():
        return "skipped", 0.0, ""
    natural = u.natural_object() if u.object_path is not None else None
    before = natural.stat().st_mtime_ns if natural is not None and natural.is_file() else None
    t0 = time.monotonic()
    code, err = runner(u.local_argv(), u.directory)
    secs = time.monotonic() - t0
    if code != 0:
        return "failed", secs, f"exit {code}: {_tail(err)}"
    if natural is None:
        return "ok", secs, ""
    if not _fresh(natural, before):
        return "failed", secs, "no object produced"
    if natural != u.object_path:
        _move(natural, u.object_path)  # type: ignore[arg-type]
    return "ok", secs, ""


def _find(root: Path, name: str) -> Optional[Path]:
    for dirpath, _dirs, files in os.walk(root):
        if name in files:
            p = Path(dirpath) / name
            if p.stat().st_size > 0:
                return p
    return None


def _watch(job: Any, tasks: list[RemoteTask], opts: BuildOptions, names: dict[str, str],
           prog: TextIO, stop: threading.Event, states: tuple[Any, Any, Any, Any]) -> dict[str, Any]:
    """Print task events until the job ends. Returns the final results by task name."""
    by_name = {t.name: t for t in tasks}
    running, placed, succeeded, failed = states
    started: set[str] = set()
    done: set[str] = set()
    for ev in job.events():
        name = getattr(ev, "task_name", "") or ""
        task = by_name.get(name)
        if task is None:
            continue
        state = getattr(ev, "state", None)
        if state in (running, placed):
            if name not in started:
                started.add(name)
                _event(prog, "RUNNING", task_label(task))
        elif state in (succeeded, failed) and name not in done:
            done.add(name)
            res = getattr(ev, "result", None)
            if state == succeeded:
                who_s = who(getattr(res, "executed_by", ""), names) if res is not None else "?"
                secs = (getattr(res, "duration_ms", 0) or 0) / 1000.0
                _event(prog, "DONE", task_label(task), f"{who_s}  {secs:.2f}s")
            else:
                detail = getattr(ev, "detail", "") or task_failure(res)
                _event(prog, "FAILED", task_label(task), explain(detail))
                if not opts.keep_going and not stop.is_set():
                    stop.set()
                    job.cancel()
    return dict(job.wait(check=False).results.items())


def _collect(job: Any, tasks: list[RemoteTask], results: dict[str, Any], fetch_root: Path,
             records: dict[int, UnitRecord], names: dict[str, str]) -> None:
    for task in tasks:
        res = results.get(task.name)
        dest = fetch_root / task.name
        fetch_err = ""
        if res is not None:
            try:
                job.fetch(task.name, dest)
            except Exception as e:  # a failed task may have nothing to fetch
                fetch_err = str(e)
        failure = task_failure(res)
        for u in task.units:
            rec = records[u.index]
            rec.task = task.name
            if res is not None:
                rec.node_id = getattr(res, "executed_by", "") or ""
                rec.node = who(rec.node_id, names)
                rec.task_wall_s = (getattr(res, "duration_ms", 0) or 0) / 1000.0
                rec.cache_hit = bool(getattr(res, "cache_hit", False))
            found = _find(dest, u.object_name)
            if found is not None and u.object_path is not None:
                _move(found, u.object_path)
                rec.status = "ok"
            else:
                rec.status = "failed"
                msg = failure or (f"fetch failed: {fetch_err}" if fetch_err else "no object produced")
                rec.error = explain(msg)


def run_build(
    plan: Plan,
    opts: BuildOptions,
    client: Any = None,
    *,
    common_pb2: Any = None,
    runner: Runner = run_process,
    progress: Optional[TextIO] = None,
) -> tuple[list[UnitRecord], float]:
    """Execute the plan. Returns the per-unit records (by plan order) and total seconds."""
    prog: TextIO = progress if progress is not None else sys.stderr
    t0 = time.monotonic()
    opts.out_dir.mkdir(parents=True, exist_ok=True)
    records = {u.index: UnitRecord.for_unit(u) for u in plan.units}
    for u in plan.units:
        if u.object_path is not None and u.object_path.is_file():
            u.object_path.unlink()  # a stale object must never pass for this run's output

    stop = threading.Event()
    names = peer_names(client) if client is not None and plan.remote_units else {}
    remote = plan.remote_units
    if opts.verbose:
        for u in remote:
            print(f"preprocess {shlex.join(u.preprocess_argv('<stage>/' + u.stage_name))}", file=prog, flush=True)

    with tempfile.TemporaryDirectory(prefix="zm-cc-") as td:
        stage = Path(td) / "in"
        (stage / "out").mkdir(parents=True)
        (stage / "out" / ".zm-keep").touch()  # an empty out/ must survive the upload
        with ThreadPoolExecutor(max_workers=opts.jobs) as pool:
            staged = list(pool.map(lambda u: _stage_unit(u, stage, runner), remote))

        ready: list[Unit] = []
        for u, (size, err) in zip(remote, staged):
            rec = records[u.index]
            if err:
                rec.status = "failed"
                rec.error = explain(err)
                _event(prog, "FAILED", u.label, rec.error)
                if not opts.keep_going:
                    stop.set()
            else:
                rec.ship_bytes = size
                ready.append(u)

        tasks: list[RemoteTask] = []
        if stop.is_set():
            for u in ready:
                records[u.index].status = "skipped"
        elif ready:
            tasks = make_tasks(ready, opts.chunk)

        job: Any = None
        results: dict[str, Any] = {}
        if tasks:
            if client is None or common_pb2 is None:
                raise RuntimeError("remote units need a daemon client")
            root = client.upload(stage, timeout=opts.submit_timeout)
            spec = build_job_spec(input_root=root, tasks=tasks, opts=opts, common_pb2=common_pb2)
            job = client.submit(spec, timeout=opts.submit_timeout)
            print(f"zm-cc: submitted job {job.job_id} ({len(tasks)} task(s))", file=prog, flush=True)

        states = (
            (common_pb2.RUNNING, common_pb2.PLACED, common_pb2.SUCCEEDED, common_pb2.FAILED)
            if common_pb2 is not None
            else (None, None, None, None)
        )
        with ThreadPoolExecutor(max_workers=opts.jobs) as pool:
            futures = [(u, pool.submit(_run_local, u, runner, stop)) for u in plan.local_units]
            if job is not None:
                results = _watch(job, tasks, opts, names, prog, stop, states)
                _collect(job, tasks, results, Path(td) / "fetch", records, names)
            for u, fut in futures:
                status, secs, err = fut.result()
                rec = records[u.index]
                rec.status = status
                rec.wall_s = secs
                if status == "failed":
                    rec.error = explain(err)
                    _event(prog, "FAILED", u.label, rec.error)
                    if not opts.keep_going:
                        stop.set()
                elif status == "ok":
                    _event(prog, "LOCAL", u.label, f"{secs:.2f}s")

    for rec in records.values():
        if rec.status == "pending":
            rec.status = "skipped"
    return [records[i] for i in sorted(records)], time.monotonic() - t0


def _link_driver(plan: Plan) -> str:
    units = [u for u in plan.units if u.object_path is not None]
    cxx = [u for u in units if u.source is not None and u.source.lang is not None and u.source.lang.family == "c++"]
    pick = cxx or units
    return pick[0].driver_local if pick else "cc"


def link_objects(plan: Plan, records: list[UnitRecord], *, output: Path, flags: str,
                 runner: Runner = run_process) -> tuple[int, str]:
    """Link the collected objects locally with the compiler that built them (C++ driver if any)."""
    objs = [r.output for r in records if r.status == "ok" and r.output]
    argv = [_link_driver(plan), *objs, *shlex.split(flags), "-o", str(output)]
    return runner(argv, Path.cwd())
