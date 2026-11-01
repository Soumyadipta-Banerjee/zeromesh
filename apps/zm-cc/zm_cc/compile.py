"""Run a Plan: compile remote units on the mesh, place the objects.

Two modes:

  * ``distcc`` (default): ``cc -E`` locally, ship preprocessed inputs, remote codegen.
  * ``tree``: upload the source tree once, remote full ``cc -c`` (no local ``-E``).

Passthrough (local) units always run here. An object is good when it exists and is
non-empty. A task can fail after some of its units have compiled, so placement is
checked per unit, not per task.
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
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Optional, Sequence, TextIO

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
    # distcc: preprocess locally, ship .i; tree: upload source tree, full remote -c
    mode: str = "distcc"
    tree_root: Optional[Path] = None


def build_job_spec(
    *,
    input_root: Any = None,
    tasks: list[RemoteTask],
    opts: BuildOptions,
    common_pb2: Any,
    input_roots: Optional[dict[str, Any]] = None,
    mem_bytes: Optional[dict[str, int]] = None,
) -> Any:
    """One TaskSpec per RemoteTask. ``common_pb2`` is passed in so tests can stub it.

    ``input_roots`` gives each task its own root by task name; without it every task shares ``input_root``.
    A donor copies the whole root into the task's work dir, so a shared root makes every task copy every file.
    """
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
                input_root=input_roots[t.name] if input_roots is not None else input_root,
                working_dir=t.working_dir or ".",
                outputs=["out/"],
                env_spec=common_pb2.EnvSpec(name=opts.env, params=params),
                resources=common_pb2.Resources(
                    cores=opts.cores, mem_bytes=(mem_bytes or {}).get(t.name, 0)
                ),
                policy=policy,
                env=env,
            )
        )
    return common_pb2.JobSpec(name="zm-cc", tasks=specs)


# cc1plus peaks near 700 MB on an 8 MB preprocessed protobuf/gRPC unit and near 250 MB on a 2 MB one.
# The estimate is what a donor reserves for the task. Keep large-TU headroom, but do not reserve
# 250MB+ for tiny Redis-sized .i files — that serialized 1.5GiB VMs to ~1 task/node.
_MEM_BASE = 96 * 1024 * 1024
_MEM_PER_INPUT_BYTE = 80


def estimate_task_mem(sizes: list[int]) -> int:
    """Memory to reserve for a task whose units compile one after another: the largest unit's."""
    return _MEM_BASE + _MEM_PER_INPUT_BYTE * max(sizes, default=0)


def _wave_task_count(opts: BuildOptions) -> int:
    """How many remote tasks to bundle per submitted job while preprocessing continues."""
    # Fill a 2-node × 2-slot mesh quickly without waiting for the whole compile_commands set.
    return max(4, opts.jobs, opts.chunk)


def _upload_task_inputs(
    client: Any, tasks: list[RemoteTask], stage: Path, scratch: Path, timeout: float, workers: int = 1
) -> tuple[dict[str, Any], dict[str, int]]:
    """Upload each task's own files as its input root, several at a time.

    Returns (task name -> root digest, task name -> memory estimate in bytes).
    """
    mems: dict[str, int] = {}

    def one(t: RemoteTask) -> tuple[str, Any]:
        tdir = scratch / t.name
        (tdir / "out").mkdir(parents=True)
        (tdir / "out" / ".zm-keep").touch()  # an empty out/ must survive the upload
        sizes = []
        for u in t.units:
            src, dst = stage / u.stage_name, tdir / u.stage_name
            sizes.append(src.stat().st_size)
            try:
                os.link(src, dst)
            except OSError:
                shutil.copyfile(src, dst)
        mems[t.name] = estimate_task_mem(sizes)
        return t.name, client.upload(tdir, timeout=timeout)

    with ThreadPoolExecutor(max_workers=max(1, min(workers, len(tasks)))) as pool:
        roots = dict(pool.map(one, tasks))
    return roots, mems


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


_SKIP_TREE_DIRS = frozenset({".git", ".hg", ".svn", "__pycache__", ".cache", "zm-objs"})
_SKIP_TREE_SUFFIXES = (
    ".o",
    ".a",
    ".so",
    ".dylib",
    ".dll",
    ".pyc",
    ".exe",
    ".d",
    ".gcda",
    ".gcno",
)


def _skip_tree_file(name: str) -> bool:
    """Build products and debug leftovers that remote ``-c`` does not need."""
    lower = name.lower()
    return lower.endswith(_SKIP_TREE_SUFFIXES) or lower.startswith("zm-objs")


def _stage_tree_root(root: Path, dest: Path, working_dirs: Sequence[str] = (".",)) -> Path:
    """Hardlink/copy ``root`` into ``dest`` and ensure ``out/`` under each task working_dir.

    Donors materialize the upload at ``/work`` and ``cd`` into ``working_dir``; outputs are
    collected from ``working_dir/out/``. An empty ``out/.zm-keep`` makes the directory survive CAS.
    Object files and archives are skipped so a previously built tree stays small to ship.
    """
    root = root.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames[:] = [d for d in dirnames if d not in _SKIP_TREE_DIRS]
        rel = os.path.relpath(dirpath, root)
        out_dir = dest if rel == "." else dest / rel
        out_dir.mkdir(parents=True, exist_ok=True)
        for name in filenames:
            if _skip_tree_file(name):
                continue
            src = Path(dirpath) / name
            dst = out_dir / name
            if dst.exists() or not src.is_file():
                continue
            try:
                os.link(src, dst)
            except OSError:
                shutil.copy2(src, dst)
    seen: set[str] = set()
    for wd in working_dirs:
        key = "." if wd in ("", ".") else wd
        if key in seen:
            continue
        seen.add(key)
        keep = (dest if key == "." else dest / key) / "out" / ".zm-keep"
        keep.parent.mkdir(parents=True, exist_ok=True)
        keep.touch()
    return dest


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
    tree_mode = opts.mode == "tree"
    if opts.verbose:
        for u in remote:
            if tree_mode and opts.tree_root is not None:
                print(
                    f"tree-compile {shlex.join(u.tree_remote_argv(opts.tree_root))}",
                    file=prog,
                    flush=True,
                )
            else:
                print(
                    f"preprocess {shlex.join(u.preprocess_argv('<stage>/' + u.stage_name))}",
                    file=prog,
                    flush=True,
                )

    with tempfile.TemporaryDirectory(prefix="zm-cc-") as td:
        tasks: list[RemoteTask] = []
        job_batches: list[tuple[Any, list[RemoteTask]]] = []  # (job, its tasks)
        results: dict[str, Any] = {}

        if tree_mode:
            if opts.tree_root is None:
                raise RuntimeError("tree mode needs BuildOptions.tree_root")
            root = opts.tree_root.resolve()
            ready = []
            for u in remote:
                rec = records[u.index]
                if u.path is None or not u.path.is_file():
                    rec.status = "failed"
                    rec.error = explain("source file missing")
                    if not opts.keep_going:
                        stop.set()
                    continue
                rec.ship_bytes = u.path.stat().st_size
                ready.append(u)
            if stop.is_set():
                for u in ready:
                    records[u.index].status = "skipped"
                ready = []
            if ready:
                tasks = make_tasks(ready, opts.chunk, mode="tree", root=root)
            if tasks:
                if client is None or common_pb2 is None:
                    raise RuntimeError("remote units need a daemon client")
                wds = [t.working_dir for t in tasks]
                staged_root = _stage_tree_root(root, Path(td) / "tree", wds)
                tree_mb = sum(f.stat().st_size for f in staged_root.rglob("*") if f.is_file()) / 1_048_576
                print(
                    f"zm-cc: tree mode — uploading workspace {root} once ({tree_mb:.1f} MB, "
                    f"no local -E) ...",
                    file=prog,
                    flush=True,
                )
                up_t0 = time.monotonic()
                shared_root = client.upload(staged_root, timeout=opts.submit_timeout)
                up_secs = time.monotonic() - up_t0
                print(
                    f"zm-cc: upload done in {up_secs:.1f}s — submitting {len(tasks)} task(s) "
                    f"(shared tree digest) ...",
                    file=prog,
                    flush=True,
                )
                mems = {
                    t.name: estimate_task_mem(
                        [u.path.stat().st_size for u in t.units if u.path is not None]
                    )
                    for t in tasks
                }
                spec = build_job_spec(
                    input_root=shared_root,
                    mem_bytes=mems,
                    tasks=tasks,
                    opts=opts,
                    common_pb2=common_pb2,
                )
                job = client.submit(spec, timeout=opts.submit_timeout)
                job_batches.append((job, tasks))
                print(f"zm-cc: submitted job {job.job_id} ({len(tasks)} task(s))", file=prog, flush=True)
        else:
            # distcc mode: preprocess, and submit waves of tasks as soon as each wave is ready
            # so donors compile while later units are still being preprocessed (classic distcc shape).
            stage = Path(td) / "in"
            (stage / "out").mkdir(parents=True)
            (stage / "out" / ".zm-keep").touch()
            if client is None or common_pb2 is None:
                if remote:
                    raise RuntimeError("remote units need a daemon client")

            n_total = len(remote)
            n_done = 0
            n_failed = 0
            pp_t0 = time.monotonic()
            wave_n = _wave_task_count(opts)
            print(
                f"zm-cc: preprocessing {n_total} unit(s) with -j{opts.jobs} "
                f"(pipeline: submit every {wave_n} task(s)) ...",
                file=prog,
                flush=True,
            )

            # Preprocess concurrently, but form tasks in *plan order* so cc_0000 is always the
            # first remote units (tests and reports stay stable). Submit a wave as soon as the
            # next plan-ordered chunk is ready — donors run while later -E continues.
            staged_ok: list[Optional[Unit]] = [None] * n_total
            staged_done = [False] * n_total
            wave: list[RemoteTask] = []
            task_seq = 0
            next_emit = 0  # next index into `remote` to consume into tasks
            tin = Path(td) / "tin"

            def _append_task(group: list[Unit]) -> None:
                nonlocal task_seq
                made = make_tasks(group, max(len(group), 1), mode="distcc")
                for t in made:
                    wave.append(
                        RemoteTask(
                            name=f"cc_{task_seq:04d}",
                            units=t.units,
                            argv=t.argv,
                            working_dir=t.working_dir,
                        )
                    )
                    task_seq += 1

            def _emit_ready_prefix(*, final: bool = False) -> None:
                """Emit plan-ordered tasks once each next chunk's units have finished -E."""
                nonlocal next_emit
                while next_emit < n_total:
                    while next_emit < n_total and staged_done[next_emit] and staged_ok[next_emit] is None:
                        next_emit += 1  # skip preprocess failures
                    if next_emit >= n_total or not staged_done[next_emit]:
                        return
                    group: list[Unit] = []
                    j = next_emit
                    while j < n_total and len(group) < opts.chunk:
                        if not staged_done[j]:
                            if final:
                                break
                            return  # wait for the rest of this chunk
                        if staged_ok[j] is None:
                            break  # emit what we have before the failure hole
                        group.append(staged_ok[j])
                        j += 1
                    if not group:
                        return
                    if len(group) < opts.chunk and not final and j < n_total and not staged_done[j]:
                        return
                    next_emit = j
                    _append_task(group)

            def _submit_wave(force: bool = False) -> None:
                nonlocal wave
                if not wave:
                    return
                if not force and len(wave) < wave_n:
                    return
                batch = list(wave)
                wave = []
                assert client is not None and common_pb2 is not None
                roots, mems = _upload_task_inputs(
                    client, batch, stage, tin / f"w{len(job_batches)}", opts.submit_timeout, workers=opts.jobs
                )
                spec = build_job_spec(
                    input_roots=roots, mem_bytes=mems, tasks=batch, opts=opts, common_pb2=common_pb2
                )
                job = client.submit(spec, timeout=opts.submit_timeout)
                job_batches.append((job, batch))
                tasks.extend(batch)
                print(
                    f"zm-cc: submitted job {job.job_id} ({len(batch)} task(s); "
                    f"{len(tasks)} remote task(s) so far)",
                    file=prog,
                    flush=True,
                )

            futures_map: dict[Any, int] = {}
            with ThreadPoolExecutor(max_workers=opts.jobs) as pool:
                for i, u in enumerate(remote):
                    futures_map[pool.submit(_stage_unit, u, stage, runner)] = i
                for fut in as_completed(futures_map):
                    if stop.is_set() and not opts.keep_going:
                        break
                    i = futures_map[fut]
                    size, err = fut.result()
                    n_done += 1
                    u = remote[i]
                    rec = records[u.index]
                    staged_done[i] = True
                    if err:
                        n_failed += 1
                        rec.status = "failed"
                        rec.error = explain(err)
                        staged_ok[i] = None
                        if not opts.keep_going:
                            stop.set()
                    else:
                        rec.ship_bytes = size
                        staged_ok[i] = u
                    _emit_ready_prefix(final=False)
                    _submit_wave(force=False)
                    print(
                        f"  preprocess [{n_done}/{n_total}]{'  FAIL' if err else '  ok  '}"
                        f"  {size // 1024:>6} KB  {u.label}",
                        file=prog,
                        flush=True,
                    )

            if stop.is_set():
                for t in wave:
                    for u in t.units:
                        if records[u.index].status == "pending":
                            records[u.index].status = "skipped"
                wave = []
                for i in range(next_emit, n_total):
                    u = remote[i]
                    if records[u.index].status == "pending":
                        records[u.index].status = "skipped"
            else:
                _emit_ready_prefix(final=True)

            _submit_wave(force=True)

            pp_secs = time.monotonic() - pp_t0
            pp_mb = sum(
                (records[u.index].ship_bytes or 0) for u in remote if records[u.index].ship_bytes
            ) / 1_048_576
            print(
                f"zm-cc: preprocess+pipeline done in {pp_secs:.1f}s  —  {pp_mb:.1f} MB staged"
                f"  ({n_failed} failed, {len(job_batches)} job(s), {len(tasks)} task(s))",
                file=prog,
                flush=True,
            )

        states = (
            (common_pb2.RUNNING, common_pb2.PLACED, common_pb2.SUCCEEDED, common_pb2.FAILED)
            if common_pb2 is not None
            else (None, None, None, None)
        )

        def _cancel_all_jobs() -> None:
            for j, _batch in job_batches:
                try:
                    j.cancel()
                except Exception:
                    pass

        def _watch_one(job: Any, batch: list[RemoteTask]) -> tuple[Any, list[RemoteTask], dict[str, Any]]:
            part = _watch(job, batch, opts, names, prog, stop, states)
            if stop.is_set() and not opts.keep_going:
                _cancel_all_jobs()
            return job, batch, part

        with ThreadPoolExecutor(max_workers=max(opts.jobs, len(job_batches) or 1)) as pool:
            futures = [(u, pool.submit(_run_local, u, runner, stop)) for u in plan.local_units]
            watch_futs = [pool.submit(_watch_one, job, batch) for job, batch in job_batches]
            for wf in as_completed(watch_futs):
                job, batch, part = wf.result()
                results.update(part)
                _collect(job, batch, part, Path(td) / "fetch", records, names)
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


def retry_failed_local(
    plan: Plan,
    records: list[UnitRecord],
    opts: BuildOptions,
    *,
    runner: Runner = run_process,
    progress: Any = None,
) -> int:
    """Compile units whose remote task failed here instead. Returns how many now succeeded.

    A donor can fail a task for reasons that have nothing to do with the source (for example the
    mirror filesystem not mounting), so one flaky donor must not fail the whole build.
    """
    by_index = {u.index: u for u in plan.units}
    redo = [r for r in records if r.status == "failed" and r.mode == "remote" and r.index in by_index]
    if not redo:
        return 0
    prog = progress if progress is not None else sys.stderr
    print(f"zm-cc: retrying {len(redo)} failed remote unit(s) locally ...", file=prog, flush=True)
    stop = threading.Event()
    fixed = 0
    with ThreadPoolExecutor(max_workers=max(1, min(opts.jobs, 4))) as pool:
        futures = [(r, pool.submit(_run_local, by_index[r.index], runner, stop)) for r in redo]
        for rec, fut in futures:
            u = by_index[rec.index]
            status, secs, err = fut.result()
            if status == "ok":
                rec.status, rec.mode, rec.wall_s = "ok", "local", secs
                rec.node, rec.node_id, rec.error = "local (retry)", "", ""
                rec.output = str(u.object_path)
                fixed += 1
                _event(prog, "LOCAL", u.label, f"retry ok {secs:.2f}s")
            else:
                rec.error = explain(err)
                _event(prog, "FAILED", u.label, f"retry failed: {rec.error}")
    return fixed
