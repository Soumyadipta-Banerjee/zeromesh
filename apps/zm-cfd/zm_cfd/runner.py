"""Submit independent OpenFOAM cases through the ZeroMesh Python SDK."""

from __future__ import annotations

import shlex
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Sequence, TextIO

from zm_cfd.cases import Case, discover_cases


@dataclass
class CfdOptions:
    cases: Path
    solver: str = "simpleFoam"
    solver_args: list[str] = field(default_factory=list)
    bashrc: Path | None = None
    env: str = "host"
    cores: int = 1
    out_dir: Path = Path("cfd-results")
    may_run_local: bool = True
    may_run_remote: bool = True


@dataclass
class CaseResult:
    case: Case
    executed_by: str = ""
    exit_code: int = -1
    error: str = ""


@dataclass
class CfdResult:
    cases: list[CaseResult]

    @property
    def failed(self) -> list[CaseResult]:
        return [case for case in self.cases if case.exit_code != 0 or case.error]


def _command(opts: CfdOptions) -> list[str]:
    command = [opts.solver, *opts.solver_args]
    if opts.bashrc is None:
        return command
    script = f"source {shlex.quote(str(opts.bashrc))} && exec {shlex.join(command)}"
    return ["bash", "-lc", script]


def build_job_spec(
    *,
    input_root: Any,
    cases: Sequence[Case],
    command: Sequence[str],
    env: str,
    cores: int,
    common_pb2: Any,
    may_run_local: bool,
    may_run_remote: bool,
) -> Any:
    """Build one changed-output-capturing task for every OpenFOAM case."""
    tasks = []
    for case in cases:
        tasks.append(
            common_pb2.TaskSpec(
                name=case.name,
                argv=list(command),
                input_root=input_root,
                working_dir=case.working_dir,
                capture_changed=True,
                env_spec=common_pb2.EnvSpec(name=env),
                resources=common_pb2.Resources(cores=cores),
                policy=common_pb2.Policy(
                    may_run_local=may_run_local,
                    may_run_remote=may_run_remote,
                ),
            )
        )
    return common_pb2.JobSpec(name="zm-cfd-openfoam-sweep", tasks=tasks)


def run_cfd(
    client: Any,
    opts: CfdOptions,
    *,
    progress_file: TextIO | None = None,
) -> CfdResult:
    """Upload cases, run one solver task per case, fetch changed result files."""
    from zeromesh.v1 import common_pb2

    out = progress_file if progress_file is not None else sys.stderr
    root = opts.cases.resolve()
    cases = discover_cases(root)
    if opts.cores < 1:
        raise ValueError("--cores must be >= 1")
    if opts.env != "host":
        raise ValueError("OpenFOAM cases currently require --env host")

    print(
        f"zm-cfd: {len(cases)} independent OpenFOAM cases "
        f"(solver={opts.solver}, env={opts.env})",
        file=out,
        flush=True,
    )
    input_root = client.upload(root)
    spec = build_job_spec(
        input_root=input_root,
        cases=cases,
        command=_command(opts),
        env=opts.env,
        cores=opts.cores,
        common_pb2=common_pb2,
        may_run_local=opts.may_run_local,
        may_run_remote=opts.may_run_remote,
    )
    job = client.submit(spec)
    print(f"zm-cfd: submitted job {job.job_id}", file=out, flush=True)
    status = job.wait(check=False)

    results: list[CaseResult] = []
    opts.out_dir.mkdir(parents=True, exist_ok=True)
    for case in cases:
        result = status.results.get(case.name)
        if result is None:
            item = CaseResult(case=case, error="task result missing")
        else:
            item = CaseResult(
                case=case,
                executed_by=result.executed_by,
                exit_code=result.exit_code,
                error=result.error,
            )
            if item.exit_code == 0 and not item.error:
                job.fetch(case.name, opts.out_dir)
        results.append(item)
        where = item.executed_by[:8] if item.executed_by else "?"
        state = "OK" if item.exit_code == 0 and not item.error else "FAILED"
        print(f"{state:6} case {case.name} → {where}", file=out, flush=True)

    return CfdResult(cases=results)
