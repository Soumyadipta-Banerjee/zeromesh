"""Submit folder tasks across the ZeroMesh mesh and collect outputs."""

from __future__ import annotations

import os
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional, TextIO

from zm_task.tasks import TaskLine, split_task_groups


@dataclass
class RunOptions:
    task_dir: Path                          # folder containing run.sh + payload
    tasks: list[TaskLine]                   # parsed executable lines
    out_dir: Path = Path("output")          # where to collect outputs
    env: str = "host"                       # env provider
    mirror_paths: list[Path] = field(default_factory=list)
    cores: int = 1
    group_size: int = 1                     # tasks per batch (sequential within batch)
    may_run_local: bool = True
    may_run_remote: bool = True
    shell: str = "/bin/sh"                  # shell to execute commands
    outputs: list[str] = field(default_factory=lambda: ["out/"])


@dataclass
class TaskProgress:
    task: TaskLine
    group_idx: int
    executed_by: str = ""
    done: bool = False


@dataclass
class RunResult:
    tasks_ok: list[TaskLine] = field(default_factory=list)
    tasks_failed: list[TaskLine] = field(default_factory=list)
    progress: list[TaskProgress] = field(default_factory=list)


class TasksFailed(RuntimeError):
    """One or more task groups failed."""

    def __init__(self, failures: list[dict], message: str):
        super().__init__(message)
        self.failures = failures


def _build_group_command(
    group: list[TaskLine],
    shell: str,
    outputs: list[str] | None = None,
) -> list[str]:
    """Build the argv for a task group.

    Creates output directories (e.g. out/) beforehand, then executes commands.
    """
    cmds: list[str] = []
    if outputs:
        # For entries like "out/" or "results/data", ensure parent directory exists
        dirs = {o if o.endswith("/") else str(Path(o).parent) for o in outputs}
        dirs.discard(".")
        if dirs:
            cmds.append(f"mkdir -p {' '.join(dirs)}")

    cmds.extend(t.command for t in group)
    combined = " && ".join(cmds)
    return [shell, "-c", combined]


def _peer_names(client: Any) -> dict[str, str]:
    """Map node_id (and 8-char prefix) → hostname."""
    out: dict[str, str] = {}
    try:
        for p in client.peers():
            nid = getattr(p, "node_id", "") or ""
            host = getattr(p, "hostname", "") or nid[:8]
            if not nid:
                continue
            out[nid] = host
            out[nid[:8]] = host
    except Exception:
        pass
    return out


def _who(node_id: str, names: dict[str, str]) -> str:
    if not node_id:
        return "?"
    host = names.get(node_id) or names.get(node_id[:8])
    if host:
        return f"{host} ({node_id[:8]})"
    return node_id[:8]


def _print_event(kind: str, group_name: str, who: str, file: TextIO, extra: str = "") -> None:
    msg = f"{kind:9} {group_name:<20}  →  {who}"
    if extra:
        msg += f"  {extra}"
    print(msg, file=file, flush=True)


def build_job_spec(
    *,
    input_root: Any,
    groups: list[list[TaskLine]],
    env: str,
    cores: int,
    shell: str,
    common_pb2: Any,
    outputs: list[str],
    may_run_local: bool = True,
    may_run_remote: bool = True,
    mirror_paths: Optional[list[str]] = None,
    task_env: Optional[dict[str, str]] = None,
) -> Any:
    """Build a JobSpec with one task per group of TaskLines."""
    policy = common_pb2.Policy(
        may_run_local=may_run_local,
        may_run_remote=may_run_remote,
    )
    env_params: dict[str, str] = {}
    if env == "mirror" and mirror_paths:
        env_params["mirror_paths"] = "\n".join(mirror_paths)

    tasks = []
    for gi, group in enumerate(groups):
        group_name = f"group_{gi}"
        argv = _build_group_command(group, shell, outputs=outputs)
        tasks.append(
            common_pb2.TaskSpec(
                name=group_name,
                argv=argv,
                input_root=input_root,
                outputs=outputs,
                env_spec=common_pb2.EnvSpec(name=env, params=env_params),
                resources=common_pb2.Resources(cores=cores),
                policy=policy,
                env=task_env or {},
            )
        )
    return common_pb2.JobSpec(name="zm-task", tasks=tasks)


def run_tasks(
    client: Any,
    opts: RunOptions,
    *,
    progress_file: TextIO | None = None,
) -> RunResult:
    """Upload task folder, submit tasks to mesh, fetch outputs."""
    from zeromesh.v1 import common_pb2

    prog = progress_file if progress_file is not None else sys.stderr
    task_dir = opts.task_dir.resolve()
    if not task_dir.is_dir():
        raise FileNotFoundError(f"task directory not found: {task_dir}")

    run_sh = task_dir / "run.sh"
    if not run_sh.is_file():
        raise FileNotFoundError(f"run.sh not found in {task_dir}")

    groups = split_task_groups(opts.tasks, opts.group_size)
    total_tasks = len(opts.tasks)
    total_groups = len(groups)

    names = _peer_names(client)
    print(
        f"zm-task: {total_tasks} commands → {total_groups} groups "
        f"(group_size={opts.group_size}, env={opts.env})",
        file=prog,
        flush=True,
    )
    for gi, group in enumerate(groups):
        labels = ", ".join(t.label for t in group)
        print(f"  queue     group_{gi:<3}  [{labels}]", file=prog, flush=True)

    # Upload the task folder to CAS
    root = client.upload(task_dir)
    mirror_paths = None
    if opts.env == "mirror":
        mirror_paths = [str(p.resolve()) for p in opts.mirror_paths]

    job_spec = build_job_spec(
        input_root=root,
        groups=groups,
        env=opts.env,
        cores=opts.cores,
        shell=opts.shell,
        common_pb2=common_pb2,
        outputs=opts.outputs,
        may_run_local=opts.may_run_local,
        may_run_remote=opts.may_run_remote,
        mirror_paths=mirror_paths,
    )
    job = client.submit(job_spec)
    print(f"zm-task: submitted job {job.job_id}", file=prog, flush=True)

    # Track progress
    progress_map: dict[str, int] = {}
    for gi in range(total_groups):
        progress_map[f"group_{gi}"] = gi

    done_count = 0
    started: set[str] = set()

    running_state = common_pb2.RUNNING
    placed_state = common_pb2.PLACED
    succeeded_state = common_pb2.SUCCEEDED

    for ev in job.events():
        name = getattr(ev, "task_name", "") or ""
        if name not in progress_map:
            continue
        state = getattr(ev, "state", None)
        detail = (getattr(ev, "detail", "") or "").strip()
        if state in (running_state, placed_state) and name not in started:
            started.add(name)
            who = detail if detail else "mesh…"
            _print_event("RUNNING", name, who, prog)
        if state == succeeded_state and name in progress_map:
            gi = progress_map[name]
            done_count += len(groups[gi])
            _print_event(
                "DONE",
                name,
                detail or "?",
                prog,
                extra=f"[{done_count}/{total_tasks} tasks]",
            )

    # Wait for completion and collect results
    status = job.wait(check=False)
    result = RunResult()
    failures: list[dict] = []
    succeeded_groups: list[int] = []

    for gi, group in enumerate(groups):
        group_name = f"group_{gi}"
        res = status.results.get(group_name)
        if res is None:
            failures.append({"group": gi, "message": "no result from daemon"})
            result.tasks_failed.extend(group)
            continue
        if res.cancelled:
            failures.append({"group": gi, "message": "cancelled"})
            result.tasks_failed.extend(group)
            continue
        if res.timed_out:
            failures.append({"group": gi, "message": "timed out"})
            result.tasks_failed.extend(group)
            continue
        if res.exit_code != 0:
            err = (res.stderr_head or b"").decode("utf-8", errors="replace").strip()
            if err:
                lines = [ln for ln in err.splitlines() if ln.strip()]
                err = " | ".join(lines[-3:]) if lines else err
            detail_msg = f"exit {res.exit_code}: {err}" if err else f"exit code {res.exit_code}"
            failures.append({"group": gi, "message": detail_msg})
            result.tasks_failed.extend(group)
            continue
        if res.error:
            failures.append({"group": gi, "message": res.error})
            result.tasks_failed.extend(group)
            continue
        succeeded_groups.append(gi)
        result.tasks_ok.extend(group)

    # Fetch outputs
    out_dir = opts.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    for gi in succeeded_groups:
        group_name = f"group_{gi}"
        staging = out_dir / f".group_{gi}"
        if staging.exists():
            shutil.rmtree(staging)
        staging.mkdir(parents=True)
        try:
            job.fetch(group_name, staging)
            # Move all files from staging into out_dir, preserving subdirs
            _merge_tree(staging, out_dir)
        except Exception as e:
            print(f"zm-task: warning: fetch {group_name} failed: {e}", file=prog, flush=True)
        finally:
            shutil.rmtree(staging, ignore_errors=True)

    # Summary
    by_host: dict[str, list[str]] = {}
    for gi, group in enumerate(groups):
        group_name = f"group_{gi}"
        res = status.results.get(group_name)
        who_id = getattr(res, "executed_by", "") if res else ""
        label = _who(who_id, names) if who_id else "?"
        by_host.setdefault(label, []).append(group_name)
    print("zm-task: who did what", file=prog, flush=True)
    for host, gnames in sorted(by_host.items()):
        print(f"  {host}: {', '.join(gnames)}", file=prog, flush=True)

    if failures:
        lines = [f"group_{f['group']}: {f['message']}" for f in failures]
        raise TasksFailed(
            failures,
            "task group(s) failed:\n  " + "\n  ".join(lines),
        )

    return result


def _merge_tree(src: Path, dst: Path) -> None:
    """Recursively merge src tree into dst, overwriting files."""
    for root, dirs, files in os.walk(src):
        rel = Path(root).relative_to(src)
        dest_dir = dst / rel
        dest_dir.mkdir(parents=True, exist_ok=True)
        for f in files:
            src_file = Path(root) / f
            dst_file = dest_dir / f
            if dst_file.exists():
                dst_file.unlink()
            shutil.move(str(src_file), str(dst_file))
